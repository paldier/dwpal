/*  *****************************************************************************
 *         File Name    : dwpal_ext.c                             	            *
 *         Description  : D-WPAL Extender control interface 		            *
 *                                                                              *
 *  *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#if defined YOCTO
#include <slibc/string.h>
#else
#include "safe_str_lib.h"
#endif

#include "dwpal_ext.h"
#include "dwpal_log.h"	//Logging

/* Dedicated thread for hostapd event callbacks */
//#define EVENT_CALLBACK_THREAD

#define PING_CHECK_TIME 3
#define RECOVERY_RETRY_TIME 1
#define COMMAND_ENDED_SOCKET "/tmp/dwpal_command_get_ended_socket"
#define EVENT_HANDLER_SOCKET "/tmp/dwpal_event_handler_socket"


typedef enum
{
	THREAD_CANCEL = 0,
	THREAD_CREATE
} DwpalThreadOperation;

typedef struct
{
	char                        interfaceType[DWPAL_INTERFACE_TYPE_STRING_LENGTH];
	char                        VAPName[DWPAL_VAP_NAME_STRING_LENGTH];
	int                         fd, fdCmdGet;
	bool                        isConnectionEstablishNeeded;
	DwpalExtHostapEventCallback hostapEventCallback;
	DwpalExtNlEventCallback     nlEventCallback, nlCmdGetCallback;
	DwpalExtNlNonVendorEventCallback nlNonVendorEventCallback;
} DwpalService;

typedef struct
{
	int    serviceIdx;
	char   VAPName[DWPAL_VAP_NAME_STRING_LENGTH];
	char   opCode[64];
	char   msg[HOSTAPD_TO_DWPAL_MSG_LENGTH];
	size_t msgStringLen;
} EventData;

typedef void *(*ThreadEntryFunc)(void *data);  /* thread entry function */


bool isCliPrintf = false;

static DwpalService *dwpalService[NUM_OF_SUPPORTED_VAPS + 1] = { [0 ... NUM_OF_SUPPORTED_VAPS ] = NULL };  /* add 1 place for NL */
static void *context[sizeof(dwpalService) / sizeof(DwpalService *)]= { [0 ... (sizeof(dwpalService) / sizeof(DwpalService *) - 1) ] = NULL };
static int dwpal_command_get_ended = (-1);
static pthread_t listenerThreadId = (pthread_t)0;
#if defined EVENT_CALLBACK_THREAD
static int dwpal_event_handler = (-1);
static pthread_t eventHandlerThreadId = (pthread_t)0;
#endif


static DWPAL_Ret interfaceIndexGet(char *interfaceType, char *VAPName, int *idx)
{
	int    i;
	size_t numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);

	*idx = 0;

	for (i=0; i < (int)numOfServices; i++)
	{
		if (dwpalService[i] == NULL)
			continue;

		if ( (!strncmp(interfaceType, dwpalService[i]->interfaceType, DWPAL_INTERFACE_TYPE_STRING_LENGTH)) &&
		     (!strncmp(VAPName, dwpalService[i]->VAPName, DWPAL_VAP_NAME_STRING_LENGTH)) )
		{
			*idx = i;
			return DWPAL_SUCCESS;
		}
	}

	return DWPAL_INTERFACE_IS_DOWN;
}


static DWPAL_Ret interfaceIndexCreate(char *interfaceType, char *VAPName, int *idx)
{
	int    i;
	size_t numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);

	*idx = 0;

	if (interfaceIndexGet(interfaceType, VAPName, idx) == DWPAL_SUCCESS)
	{
		console_printf("%s; the interface (interfaceType= '%s', VAPName= '%s') is already exist ==> cont...\n",
		            __FUNCTION__, interfaceType, VAPName);
		return DWPAL_INTERFACE_ALREADY_UP;
	}

	/* Getting here means that the interface does NOT exist ==> create it! */
	for (i=0; i < (int)numOfServices; i++)
	{
		if (dwpalService[i] == NULL)
		{  /* First empty entry ==> use it */
			dwpalService[i] = (DwpalService *)malloc(sizeof(DwpalService));
			if (dwpalService[i] == NULL)
			{
				console_printf("%s; malloc failed ==> Abort!\n", __FUNCTION__);
				return DWPAL_FAILURE;
			}

			strcpy_s(dwpalService[i]->interfaceType, sizeof(dwpalService[i]->interfaceType), interfaceType);
			strcpy_s(dwpalService[i]->VAPName, sizeof(dwpalService[i]->VAPName), VAPName);

			*idx = i;
			return DWPAL_SUCCESS;
		}
	}

	console_printf("%s; number of interfaces (%d) reached its limit ==> Abort!\n", __FUNCTION__, i);

	return DWPAL_FAILURE;
}


static bool isAnyInterfaceActive(void)
{
	int i, numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);;

	/* check if there are active interfaces */
	for (i=0; i < numOfServices; i++)
	{
		/* In case that there is a valid context, break! */
		if (context[i] != NULL)
		{
			return true;
		}
	}

	return false;
}


static DWPAL_Ret socket_data_send(char *socketPrefixName, char *data, size_t size)
{
    int                fd = -1, byte;
	struct sockaddr_un un;
	size_t             len;
	char               socketName[SOCKET_NAME_LENGTH] = "\0";
	pid_t              pid = getpid();

	//console_printf("%s Entry; socketPrefixName= '%s', size= %d\n", __FUNCTION__, socketPrefixName, size);

	/* create a UNIX domain stream socket */
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		console_printf("%s; create socket fail; pid= %d; errno= %d ('%s')\n", __FUNCTION__, pid, errno, strerror(errno));
		return DWPAL_FAILURE;
    }

	/* fill socket address structure with server's address */
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;

	snprintf(socketName, sizeof(socketName) - 1, "%s_%d", socketPrefixName, pid);
	strcpy_s(un.sun_path, sizeof(un.sun_path), socketName);
	len = offsetof(struct sockaddr_un, sun_path) + strnlen_s(socketName, sizeof(socketName));

	if (connect(fd, (struct sockaddr *)&un, len) < 0)
	{
		console_printf("%s; connect() fail; pid= %d; errno= %d ('%s')\n",
		       __FUNCTION__, pid, errno, strerror(errno));

		if (close(fd) == (-1))
		{
			console_printf("%s; close() fail; pid= %d; errno= %d ('%s')\n",
				   __FUNCTION__, pid, errno, strerror(errno));
		}

		return DWPAL_FAILURE;
	}

	if ((byte = write(fd, data, size)) == -1)
	{
		console_printf("%s; write() fail; pid= %d; errno= %d ('%s')\n",
		       __FUNCTION__, pid, errno, strerror(errno));

		if (close(fd) == (-1))
		{
			console_printf("%s; close() fail; pid= %d; errno= %d ('%s')\n",
				   __FUNCTION__, pid, errno, strerror(errno));
		}

		return DWPAL_FAILURE;
	}

	if (close(fd) == (-1))
	{
		console_printf("%s; close() fail; pid= %d; errno= %d ('%s')\n",
		       __FUNCTION__, pid, errno, strerror(errno));
	}

	return DWPAL_SUCCESS;
}


static void interfacesRecoverIfNeeded(void)
{
	int  i, numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);

	//console_printf("%s Entry\n", __FUNCTION__);

	for (i=0; i < numOfServices; i++)
	{
		/* In case that there is no valid context, continue... */
		if (dwpalService[i] == NULL)
		{
			continue;
		}

		/* In case of recovery needed, try recover; in case of interface init, try to establish the connection */
		if ( (!strncmp(dwpalService[i]->interfaceType, "hostap", 7)) &&
		     (dwpalService[i]->isConnectionEstablishNeeded == true) )
		{
			if (dwpal_hostap_interface_attach(&context[i] /*OUT*/, dwpalService[i]->VAPName, NULL /*use one-way interface*/) == DWPAL_SUCCESS)
			{
				console_printf("%s; VAPName= '%s' interface recovered successfully!\n", __FUNCTION__, dwpalService[i]->VAPName);
				dwpalService[i]->isConnectionEstablishNeeded = false;

#if defined EVENT_CALLBACK_THREAD
				{
					EventData eventData;
					eventData.serviceIdx = i;
					strcpy_s(eventData.VAPName, sizeof(eventData.VAPName), dwpalService[i]->VAPName);
					strcpy_s(eventData.opCode, sizeof(eventData.opCode), "INTERFACE_RECONNECTED_OK");
					memset(eventData.msg, '\0', sizeof(eventData.msg));
					eventData.msgStringLen = sizeof("INTERFACE_RECONNECTED_OK");

					/* Notify via the callback that interface recovered successfully */
					if (socket_data_send(EVENT_HANDLER_SOCKET, (char *)&eventData, sizeof(EventData)) == DWPAL_FAILURE)
					{
						console_printf("%s; socket_data_send failed ==> cont...\n", __FUNCTION__);
					}
				}
#else
				/* Notify via the callback that interface recovered successfully */
				dwpalService[i]->hostapEventCallback(dwpalService[i]->VAPName, "INTERFACE_RECONNECTED_OK", NULL, 0);
#endif
			}
		}
	}
}


static void interfacesPingCheck(void)
{
	int  i, numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);
	bool isExist;

	//console_printf("%s Entry\n", __FUNCTION__);

	for (i=0; i < numOfServices; i++)
	{
		/* In case that there is no valid context, continue... */
		if (context[i] == NULL)
		{
			continue;
		}

		if (!strncmp(dwpalService[i]->interfaceType, "hostap", 7))
		{
			if (dwpalService[i]->fd > 0)
			{
				/* check if interface that should exist, still exists */
				if (dwpal_hostap_is_socket_alive(context[i], &isExist /*OUT*/) == DWPAL_FAILURE)
				{
					console_printf("%s; dwpal_hostap_is_socket_alive for VAPName= '%s' error ==> cont...\n", __FUNCTION__, dwpalService[i]->VAPName);
					continue;
				}

				if (isExist == false)
				{
					console_printf("%s; VAPName= '%s' interface needs to be recovered\n", __FUNCTION__, dwpalService[i]->VAPName);

					/* Close 'wpaCtrlPtr' and if needed also 'listenerWpaCtrlPtr', and free 'context' */
					if (dwpal_hostap_socket_close(&context[i]) == DWPAL_FAILURE)
					{
						console_printf("%s; dwpal_hostap_socket_close returned error ==> cont...\n", __FUNCTION__);
					}

					dwpalService[i]->isConnectionEstablishNeeded = true;
					dwpalService[i]->fd = -1;
				}
			}
		}
	}
}


#if defined EVENT_CALLBACK_THREAD
static void *eventHandlerThreadStart(void *temp)
{
	int                clientfd = -1, byte;
	size_t             len;
	char               rcv_buf[sizeof(EventData)];
	EventData          *eventData = NULL;
	struct sockaddr_un un;

	(void)temp;

	console_printf("%s Entry\n", __FUNCTION__);

	if (dwpal_event_handler == -1)
	{
		console_printf("%s; dwpal_event_handler not valid ==> Abort!\n", __FUNCTION__);
		return NULL;
	}

	/* Receive the msg */
	while (true)
	{
		/* Handle the COMMAND_SOCKET */
		len = sizeof(un);
		if ((clientfd = accept(dwpal_event_handler, (struct sockaddr *)&un, &len)) < 0)
		{ // wait for client to connect to me (server)
			console_printf("%s; ERROR: accept() received; clientfd= %d; errno= %d ('%s') ==> cont...\n", __FUNCTION__, clientfd, errno, strerror(errno));
			continue;
		}

		memset(rcv_buf, '\0', sizeof(rcv_buf));

		if ((byte = read(clientfd, rcv_buf, sizeof(rcv_buf))) == -1)
		{   // wait for client to write/send data to me (server)
			console_printf("%s; read() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));

			if (close(clientfd) == (-1))
			{
				console_printf("%s; close() fail; clientfd= %d; errno= %d ('%s')\n", __FUNCTION__, clientfd, errno, strerror(errno));
			}
			continue;
		}

		if (close(clientfd) == (-1))
		{
			console_printf("%s; close() fail; clientfd= %d; errno= %d ('%s')\n", __FUNCTION__, clientfd, errno, strerror(errno));
		}

		if ( (byte > (int)sizeof(EventData)) || (byte < 1) )
		{
			console_printf("%s; ERROR: read() returned buffer size (%d) out of range (1 <-> %d) ==> cont...\n", __FUNCTION__, byte, sizeof(EventData));
			continue;
		}

		eventData = (EventData *)rcv_buf;

		dwpalService[eventData->serviceIdx]->hostapEventCallback(eventData->VAPName, eventData->opCode, eventData->msg, eventData->msgStringLen);
	}

	return NULL;
}
#endif


static void *listenerThreadStart(void *temp)
{
	int     i, highestValFD, ret, numOfServices = sizeof(dwpalService) / sizeof(DwpalService *);
	bool    isInterfacesPingCheck;
	char    *msg;
	size_t  msgLen, msgStringLen;
	fd_set  rfds;
	char    opCode[64];
	struct  timeval tv;
	time_t	last_ping_check = time(NULL);
	time_t	last_recovery_time = last_ping_check;
	time_t  current_time;

	(void)temp;

	console_printf("%s Entry\n", __FUNCTION__);

	/* Receive the msg */
	while (true)
	{
		FD_ZERO(&rfds);
		highestValFD = 0;

		for (i=0; i < numOfServices; i++)
		{
			/* In case that there is no valid context, continue... */
			if (context[i] == NULL)
			{
				continue;
			}

			if (!strncmp(dwpalService[i]->interfaceType, "hostap", 7))
			{
				if (dwpal_hostap_event_fd_get(context[i], &dwpalService[i]->fd) == DWPAL_FAILURE)
				{
					/*console_printf("%s; dwpal_hostap_event_fd_get returned error ==> cont. (VAPName= '%s')\n",
					       __FUNCTION__, dwpalService[i]->VAPName);*/
					continue;
				}

				if (dwpalService[i]->fd > 0)
				{
					FD_SET(dwpalService[i]->fd, &rfds);
					highestValFD = (dwpalService[i]->fd > highestValFD)? dwpalService[i]->fd : highestValFD;  /* find the highest value fd */
				}
			}
			else if (!strncmp(dwpalService[i]->interfaceType, "Driver", 7))
			{
				if (dwpal_driver_nl_fd_get(context[i], &dwpalService[i]->fd, &dwpalService[i]->fdCmdGet) == DWPAL_FAILURE)
				{
					/*console_printf("%s; dwpal_driver_nl_fd_get returned error ==> cont. (VAPName= '%s')\n",
					       __FUNCTION__, dwpalService[i].VAPName);*/
					continue;
				}

				if (dwpalService[i]->fd > 0)
				{
					FD_SET(dwpalService[i]->fd, &rfds);
					highestValFD = (dwpalService[i]->fd > highestValFD)? dwpalService[i]->fd : highestValFD;  /* find the highest value fd */
				}

				if (dwpalService[i]->fdCmdGet > 0)
				{
					FD_SET(dwpalService[i]->fdCmdGet, &rfds);
					highestValFD = (dwpalService[i]->fdCmdGet > highestValFD)? dwpalService[i]->fdCmdGet : highestValFD;  /* find the highest value fd */
				}
			}
		}

		//console_printf("%s; highestValFD= %d\n", __FUNCTION__, highestValFD);

		/* Interval of time in which the select() will be released */
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		isInterfacesPingCheck = false;

		/* In case that no active hostap is available, highestValFD is '0' and we'll loop out according to tv values */
		ret = select(highestValFD + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0)
		{
			console_printf("%s; select() return value= %d ==> cont...; errno= %d ('%s')\n", __FUNCTION__, ret, errno, strerror(errno));
			continue;
		}

		for (i=0; i < numOfServices; i++)
		{
			/* In case that there is no valid context, continue... */
			if (context[i] == NULL)
			{
				continue;
			}

			if (!strncmp(dwpalService[i]->interfaceType, "hostap", 7))
			{
				if (dwpalService[i]->fd > 0)
				{
					if (FD_ISSET(dwpalService[i]->fd, &rfds))
					{
						/*console_printf("%s; event received; interfaceType= '%s', VAPName= '%s'\n",
						       __FUNCTION__, dwpalService[i]->interfaceType, dwpalService[i]->VAPName);*/

						msg = (char *)malloc((size_t)(HOSTAPD_TO_DWPAL_MSG_LENGTH * sizeof(char)));
						if (msg == NULL)
						{
							console_printf("%s; invalid input ('msg') parameter ==> cont...\n", __FUNCTION__);
							continue;
						}

						memset(msg, 0, HOSTAPD_TO_DWPAL_MSG_LENGTH * sizeof(char));  /* Clear the output buffer */
						memset(opCode, 0, sizeof(opCode));
						msgLen = HOSTAPD_TO_DWPAL_MSG_LENGTH - 1;  //was "msgLen = HOSTAPD_TO_DWPAL_MSG_LENGTH;"

						if (dwpal_hostap_event_get(context[i], msg /*OUT*/, &msgLen /*IN/OUT*/, opCode /*OUT*/) == DWPAL_FAILURE)
						{
							console_printf("%s; dwpal_hostap_event_get ERROR; VAPName= '%s', msgLen= %d\n",
							       __FUNCTION__, dwpalService[i]->VAPName, msgLen);

							/* Trigger the recovery check/perform immediately */
							isInterfacesPingCheck = true;
						}
						else
						{
							//console_printf("%s; msgLen= %d, msg= '%s'\n", __FUNCTION__, msgLen, msg);
//strcpy(msg, "<3>AP-STA-CONNECTED wlan0 24:77:03:80:5d:90 SignalStrength=-49 SupportedRates=2 4 11 22 12 18 24 36 48 72 96 108 HT_CAP=107E HT_MCS=FF FF FF 00 00 00 00 00 00 00 C2 01 01 00 00 00 VHT_CAP=03807122 VHT_MCS=FFFA 0000 FFFA 0000 btm_supported=1 nr_enabled=0 non_pref_chan=81:200:1:7 non_pref_chan=81:100:2:9 non_pref_chan=81:200:1:7 non_pref_chan=81:100:2:5 cell_capa=1 assoc_req=00003A01000A1B0E04606C722002E833000A1B0E0460C04331060200000E746573745F737369645F69736172010882848B960C12182432043048606C30140100000FAC040100000FAC040100000FAC020000DD070050F2020001002D1AEF1903FFFFFF00000000000000000000000000000018040109007F080000000000000040BF0CB059C103EAFF1C02EAFF1C02C70122");
//strcpy(msg, "<3>AP-STA-DISCONNECTED wlan0 14:d6:4d:ac:36:70");
//strcpy(opCode, "AP-STA-CONNECTED");

							msgStringLen = strnlen_s(msg, HOSTAPD_TO_DWPAL_MSG_LENGTH);
							//console_printf("%s; opCode= '%s', msg= '%s'\n", __FUNCTION__, opCode, msg);
							if (strncmp(opCode, "", 1))
							{
#if defined EVENT_CALLBACK_THREAD
								EventData eventData;
								eventData.serviceIdx = i;
								strcpy_s(eventData.VAPName, sizeof(eventData.VAPName), dwpalService[i]->VAPName);
								strcpy_s(eventData.opCode, sizeof(eventData.opCode), opCode);
								strcpy_s(eventData.msg, sizeof(eventData.msg), msg);
								eventData.msgStringLen = msgStringLen;

								/* Send the event via the callback */
								if (socket_data_send(EVENT_HANDLER_SOCKET, (char *)&eventData, sizeof(EventData)) == DWPAL_FAILURE)
								{
									console_printf("%s; socket_data_send failed ==> cont...\n", __FUNCTION__);
								}
#else
								dwpalService[i]->hostapEventCallback(dwpalService[i]->VAPName, opCode, msg, msgStringLen);
#endif
							}
						}

						free((void *)msg);
					}
				}
			}
			else if (!strncmp(dwpalService[i]->interfaceType, "Driver", 7))
			{
				if ( (dwpalService[i]->fd > 0) && (FD_ISSET(dwpalService[i]->fd, &rfds)) )
				{
					console_printf("%s; event received; interfaceType= '%s', VAPName= '%s'\n",
						   __FUNCTION__, dwpalService[i]->interfaceType, dwpalService[i]->VAPName);

					if (dwpal_driver_nl_msg_get(context[i], DWPAL_NL_UNSOLICITED_EVENT, dwpalService[i]->nlEventCallback, dwpalService[i]->nlNonVendorEventCallback) == DWPAL_FAILURE)
					{
						console_printf("%s; dwpal_driver_nl_msg_get ERROR\n", __FUNCTION__);
					}
				}
				else if ( (dwpalService[i]->fdCmdGet > 0) && (FD_ISSET(dwpalService[i]->fdCmdGet, &rfds)) )
				{
					console_printf("%s; 'get command' event received; interfaceType= '%s', VAPName= '%s'\n",
						   __FUNCTION__, dwpalService[i]->interfaceType, dwpalService[i]->VAPName);

					if (dwpal_driver_nl_msg_get(context[i], DWPAL_NL_SOLICITED_EVENT, dwpalService[i]->nlCmdGetCallback, NULL) == DWPAL_FAILURE)
					{
						console_printf("%s; dwpal_driver_nl_msg_get ERROR\n", __FUNCTION__);
					}
				}
			}
		}

		current_time = time(NULL);
		if ( (isInterfacesPingCheck) || ((current_time - last_ping_check) >= PING_CHECK_TIME) )
		{
			last_ping_check = current_time;
			interfacesPingCheck();
		}

		if (current_time - last_recovery_time >= RECOVERY_RETRY_TIME)
		{
			last_recovery_time = current_time;
			interfacesRecoverIfNeeded();
		}
	}

	return NULL;
}


static DWPAL_Ret listenerThreadCreate(pthread_t *thread_id, ThreadEntryFunc threadEntryFunc)
{
	int            ret;
	DWPAL_Ret      dwpalRet = DWPAL_SUCCESS;
	pthread_attr_t attr;
	size_t         stack_size = 4096;
	//void           *res;

	console_printf("%s Entry\n", __FUNCTION__);

	ret = pthread_attr_init(&attr);
	if (ret != 0)
	{
		console_printf("%s; pthread_attr_init ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
		return DWPAL_FAILURE;
	}

	ret = pthread_attr_setstacksize(&attr, stack_size);
	if (ret == -1)
	{
		console_printf("%s; pthread_attr_setstacksize ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
		dwpalRet = DWPAL_FAILURE;
	}

	if (dwpalRet == DWPAL_SUCCESS)
	{
		console_printf("%s; call pthread_create\n", __FUNCTION__);
		ret = pthread_create(thread_id, &attr, threadEntryFunc, NULL /*can be used to send params*/);
		if (ret != 0)
		{
			console_printf("%s; pthread_create ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
			dwpalRet = DWPAL_FAILURE;
		}
		console_printf("%s; return from call pthread_create, ret= %d\n", __FUNCTION__, ret);

		if (dwpalRet == DWPAL_SUCCESS)
		{
#if 0
			/* Causing the thread to be joined with the main process;
			   meaning, the process will suspend due to the thread suspend.
			   Otherwise, when process ends, the thread ends as well (although it is suspended and waiting ) */
			ret = pthread_join(*thread_id, &res);
			if (ret != 0)
			{
				console_printf("%s; pthread_join ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
				dwpalRet = DWPAL_FAILURE;
			}

			free(res);  /* Free memory allocated by thread */
#endif
		}
	}

	/* Destroy the thread attributes object, since it is no longer needed */
	ret = pthread_attr_destroy(&attr);
	if (ret != 0)
	{
		console_printf("%s; pthread_attr_destroy ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
		dwpalRet = DWPAL_FAILURE;
	}

	/* sleep for 100 ms - it is needed in case of a loop of thread create/cancel */
	usleep(100000 /*micro-sec*/);

	return dwpalRet;
}


static DWPAL_Ret threadSet(pthread_t *thread_id, DwpalThreadOperation threadOperation, ThreadEntryFunc threadEntryFunc)
{
	int ret;

	switch (threadOperation)
	{
		case THREAD_CREATE:
			if (*thread_id == 0)
			{
				if (threadEntryFunc == NULL)
				{
					return DWPAL_FAILURE;
				}

				return listenerThreadCreate(thread_id, threadEntryFunc);
			}
			break;

		case THREAD_CANCEL:
			if (*thread_id != 0)
			{
				if ( (ret = pthread_cancel(*thread_id)) != 0 )
				{
					console_printf("%s; pthread_attr_destroy ERROR (ret= %d) ==> Abort!\n", __FUNCTION__, ret);
					return DWPAL_FAILURE;
				}

				*thread_id = 0;
			}
			break;

		default:
			console_printf("%s; threadOperation (%d) not supported ==> Abort!\n", __FUNCTION__, threadOperation);
			return DWPAL_FAILURE;
			break;
	}

	return DWPAL_SUCCESS;
}


static DWPAL_Ret dwpal_command_get_ended_socket_wait(bool *isReceived, size_t *outLen, unsigned char *outData)
{
	int    res, clientfd = -1, byte;
	fd_set rfds;
	size_t len;
	struct timeval tv;
	struct sockaddr_un un;

	if ( (isReceived == NULL) || (outLen == NULL) || (outData == NULL) )
	{
		console_printf("%s; isReceived and/or outLen and/or outData is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	if (dwpal_command_get_ended <= 0)
	{
		console_printf("%s; dwpal_command_get_ended= %d ==> Abort!\n", __FUNCTION__, dwpal_command_get_ended);
		return DWPAL_FAILURE;
	}

	*outLen = 0;
	*isReceived = false;

	/* Receive the msg */
	while (true)
	{
		FD_ZERO(&rfds);
		FD_SET(dwpal_command_get_ended, &rfds);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		res = select(dwpal_command_get_ended + 1, &rfds, NULL, NULL, &tv);
		if (res < 0)
		{
			console_printf("%s; select() return value= %d ==> cont...; errno= %d ('%s') ==> expected behavior when 'Interrupted system call'\n", __FUNCTION__, res, errno, strerror(errno));
			continue;
		}

		if (FD_ISSET(dwpal_command_get_ended, &rfds))
		{  /* the select() was triggered due to the above daemon fd */
			len = sizeof(un);
			if ((clientfd = accept(dwpal_command_get_ended, (struct sockaddr *)&un, &len)) < 0)
			{  /* wait for client to be connected to me (server) */
				console_printf("%s; ERROR: accept() received; errno= %d ('%s') ==> cont...\n", __FUNCTION__, errno, strerror(errno));
				continue;
			}

			console_printf("%s; right event indication received ==> break\n", __FUNCTION__);
			byte = read(clientfd, (char *)outData, DRIVER_NL_TO_DWPAL_MSG_LENGTH);
			if ( (byte == -1) || (byte > DRIVER_NL_TO_DWPAL_MSG_LENGTH) || (byte < 1) )
			{   // wait for client to write/send data to me (server)
				console_printf("%s; read() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));

				if (close(clientfd) == (-1))
				{
					console_printf("%s; close() fail; clientfd= %d; errno= %d ('%s')\n", __FUNCTION__, clientfd, errno, strerror(errno));
				}

				continue;
			}

			*outLen = (size_t)byte;
			*isReceived = true;

			if (close(clientfd) == (-1))
			{
				console_printf("%s; close() fail; clientfd= %d; errno= %d ('%s')\n", __FUNCTION__, clientfd, errno, strerror(errno));
			}

			break;
		}

		console_printf("%s; the right event indication was NOT received ==> break\n", __FUNCTION__);
		break;
	}

	return DWPAL_SUCCESS;
}


static int fdDaemonSet(char *socketName, int *fd /* output param */)
{
	struct sockaddr_un un;
	size_t len;

	if ((*fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		console_printf("%s; create socket fail; socketName= '%s'; errno= %d ('%s')\n", __FUNCTION__, socketName, errno, strerror(errno));
		return DWPAL_FAILURE;
    }

	console_printf("%s; fd_daemon (socketName='%s') = %d\n", __FUNCTION__, socketName, *fd);

	unlink(socketName);   /* in case it already exists */

    /* fill in socket address structure */
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy_s(un.sun_path, sizeof(un.sun_path), socketName);
	len = offsetof(struct sockaddr_un, sun_path) + strnlen_s(socketName, SOCKET_NAME_LENGTH);

    /* bind the name to the descriptor */
	if (bind(*fd, (struct sockaddr *)&un, len) < 0)  // check if can use connect() instead...
	{
		console_printf("%s; bind() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));

		if (close(*fd) == (-1))
		{
			console_printf("%s; close() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));
		}

		return DWPAL_FAILURE;
    }

	if (chmod(socketName, 0600) < 0)
	{
		console_printf("%s; FAIL to chmod '%s' to 0666\n", __FUNCTION__, socketName);

		if (close(*fd) == (-1))
		{
			console_printf("%s; close() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));
		}

		return DWPAL_FAILURE;
    }

	if (listen(*fd, 10 /*Q Length*/) < 0)
	{ /* tell kernel we're a server */
		console_printf("%s; listen fail\n", __FUNCTION__);

		if (close(*fd) == (-1))
		{
			console_printf("%s; close() fail; errno= %d ('%s')\n", __FUNCTION__, errno, strerror(errno));
		}

		return DWPAL_FAILURE;
	}

	return DWPAL_SUCCESS;
}


static DWPAL_Ret dwpal_socket_create(int *fd, char *socketPrefixName)
{
	pid_t pid = getpid();
	char  socketName[SOCKET_NAME_LENGTH] = "\0";

	snprintf(socketName, sizeof(socketName) - 1, "%s_%d", socketPrefixName, pid);

	if (*fd > 0)
	{
		console_printf("%s; *fd (%d) ==> cont...\n", __FUNCTION__, *fd);
		return DWPAL_SUCCESS;
	}

	if (fdDaemonSet(socketName, fd /*output*/) == DWPAL_FAILURE)
	{
		console_printf("%s; ERROR; *fd= %d\n", __FUNCTION__, *fd);
		return DWPAL_FAILURE;
	}

	return DWPAL_SUCCESS;
}


static DWPAL_Ret nlCmdGetCallback(char *ifname, int event, int subevent, size_t len, unsigned char *data)
{
	console_printf("%s Entry; ifname= '%s', event= %d, subevent= %d (len= %d)\n", __FUNCTION__, ifname, event, subevent, len);

	(void)ifname;
	(void)event;
	(void)subevent;

	if ( (len == 0) || (data == NULL) )
	{
		console_printf("%s; len=0 and/or data is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	{
		int i;
		size_t lenToPrint = (len <= 10)? len : 10;

		console_printf("%s; Output data from the 'get' function:\n", __FUNCTION__);
		for (i=0; i < (int)lenToPrint; i++)
		{
			console_printf(" 0x%x", data[i]);
		}
		console_printf("\n");
	}

	console_printf("%s; 'get command' (event= %d) was received ==> notify dwpal_ext\n", __FUNCTION__, event);
	if (socket_data_send(COMMAND_ENDED_SOCKET, (char *)data, len) == DWPAL_FAILURE)
	{
		console_printf("%s; socket_data_send failed ==> cont...\n", __FUNCTION__);
	}

	return DWPAL_SUCCESS;
}


static DWPAL_Ret nl_cmd_handle(char *ifname,
                               unsigned int nl80211Command,
							   CmdIdType cmdIdType,
							   unsigned int subCommand,
							   unsigned char *vendorData,
							   size_t vendorDataSize,
							   size_t *outLen,
							   unsigned char *outData)
{
	int    i, idx;
	bool   isReceived = false;

	console_printf("%s; ifname= '%s', nl80211Command= 0x%x, cmdIdType= %d, subCommand= 0x%x, vendorDataSize= %d, outLen= 0x%x, outData= 0x%x\n",
	            __FUNCTION__, ifname, nl80211Command, cmdIdType, subCommand, vendorDataSize, (unsigned int)outLen, (unsigned int)outData);

	for (i=0; i < (int)vendorDataSize; i++)
	{
		console_printf("%s; vendorData[%d]= 0x%x\n", __FUNCTION__, i, vendorData[i]);
	}

	if (interfaceIndexGet("Driver", "ALL", &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	if ( (outLen != NULL) && (outData != NULL) )
	{
		/* Handle a command which invokes an event with the output data */
		if (dwpal_driver_nl_cmd_send(context[idx],
									 DWPAL_NL_SOLICITED_EVENT,
									 ifname,
									 nl80211Command,
									 cmdIdType,
									 subCommand,
									 vendorData,
									 vendorDataSize) == DWPAL_FAILURE)
		{
			console_printf("%s; dwpal_driver_nl_cmd_send returned ERROR ==> Abort!\n", __FUNCTION__);
			return DWPAL_FAILURE;
		}

		if (dwpal_command_get_ended_socket_wait(&isReceived, outLen, outData) == DWPAL_FAILURE)
		{
			console_printf("%s; dwpal_command_get_ended_socket_wait ERROR (subCommand= 0x%x) ==> Abort!\n", __FUNCTION__, subCommand);
			*outLen = 0;
			return DWPAL_FAILURE;
		}

		if (isReceived == true)
		{
			console_printf("%s; 'get command' (subCommand= 0x%x, outLen= %d) was received\n", __FUNCTION__, subCommand, *outLen);
		}
		else
		{
			console_printf("%s; 'get command' (subCommand= 0x%x) was NOT received ==> Abort!\n", __FUNCTION__, subCommand);
			*outLen = 0;
			return DWPAL_FAILURE;
		}

		return DWPAL_SUCCESS;
	}
	else
	{
		return dwpal_driver_nl_cmd_send(context[idx],
										DWPAL_NL_UNSOLICITED_EVENT,
										ifname,
										nl80211Command,
										cmdIdType,
										subCommand,
										vendorData,
										vendorDataSize);
	}
}


/* APIs */

/*! \file dwpal_ext.c
    \brief DWPAL library
    
	The purpose of this document is to describe the new dynamic configuration infrastructure, 
	* AKA D-WPAL (Dynamic Wireless Platform Abstraction Layer).
	In general, the main purpose of the new infrastructure is to have all dynamic configuration APIs (commands, reports, statistics, etc.),
	in one shared library. Dynamic configuration of hostapd (via wpa_ctrl) and the Driver (via nl).
	The library can be used by and linked with any other software clients, and will run under the caller's context, 
	* i.e. the caller's own process/thread.
	The role of D-WPAL is to provide:
		Interface to / from hostapd / Driver.	
		Parsing services.
*/

DWPAL_Ret dwpal_ext_driver_nl_scan_dump(char *ifname, DWPAL_nlNonVendorEventCallback nlEventCallback)
{
	int idx;

	console_printf("%s; ifname= '%s'\n", __FUNCTION__, ifname);

	if (interfaceIndexGet("Driver", "ALL", &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	return dwpal_driver_nl_scan_dump(context[idx], ifname, nlEventCallback);
}


DWPAL_Ret dwpal_ext_driver_nl_scan_trigger(char *ifname, ScanParams *scanParams)
{
	int idx;

	console_printf("%s; ifname= '%s'\n", __FUNCTION__, ifname);

	if (interfaceIndexGet("Driver", "ALL", &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	return dwpal_driver_nl_scan_trigger(context[idx], ifname, scanParams);
}

/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_driver_nl_get(char *ifname, unsigned int nl80211Command, CmdIdType cmdIdType, unsigned int subCommand, unsigned char *vendorData, size_t vendorDataSize, size_t *outLen, unsigned char *outData)
 **************************************************************************
 *  \brief driver-NL get command
 *  \param[in] char *ifname - the radio interface
 *  \param[in] unsigned int nl80211Command - NL 80211 command. Note: currently we support ONLY NL80211_CMD_VENDOR (0x67)
 *  \param[in] CmdIdType cmdIdType - The command ID type: NETDEV, PHY or WDEV
 *  \param[in] unsigned int subCommand - the vendor's sub-command
 *  \param[in] unsigned char *vendorData - the vendor's data (can be NULL)
 *  \param[in] size_t vendorDataSize - the vendor's data length (if the vendor data is NULL, it should be '0')
 *  \param[out] size_t *outLen - The length of the returned data
 *  \param[out] unsigned char *outData - Pointer the returned data itself
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_driver_nl_get(char *ifname, unsigned int nl80211Command, CmdIdType cmdIdType, unsigned int subCommand, unsigned char *vendorData, size_t vendorDataSize, size_t *outLen, unsigned char *outData)
{
	console_printf("%s; ifname= '%s', nl80211Command= 0x%x, cmdIdType= %d, subCommand= 0x%x\n", __FUNCTION__, ifname, nl80211Command, cmdIdType, subCommand);

	return nl_cmd_handle(ifname, nl80211Command, cmdIdType, subCommand, vendorData, vendorDataSize, outLen, outData);
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_driver_nl_cmd_send(char *ifname, unsigned int nl80211Command, CmdIdType cmdIdType, unsigned int subCommand, unsigned char *vendorData, size_t vendorDataSize)
 **************************************************************************
 *  \brief driver-NL send command
 *  \param[in] char *ifname - the radio interface
 *  \param[in] unsigned int nl80211Command - NL 80211 command. Note: currently we support ONLY NL80211_CMD_VENDOR (0x67)
 *  \param[in] CmdIdType cmdIdType - The command ID type: NETDEV, PHY or WDEV
 *  \param[in] unsigned int subCommand - the vendor's sub-command
 *  \param[in] unsigned char *vendorData - the vendor's data (can be NULL)
 *  \param[in] size_t vendorDataSize - the vendor's data length (if the vendor data is NULL, it should be '0')
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_driver_nl_cmd_send(char *ifname, unsigned int nl80211Command, CmdIdType cmdIdType, unsigned int subCommand, unsigned char *vendorData, size_t vendorDataSize)
{
	if ( (ifname == NULL) || ((vendorDataSize > 0) && (vendorData == NULL)) )
	{
		console_printf("%s; ifname or vendorData is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	console_printf("%s; ifname= '%s', nl80211Command= 0x%x, cmdIdType= %d, subCommand= 0x%x, vendorDataSize= %d\n", __FUNCTION__, ifname, nl80211Command, cmdIdType, subCommand, vendorDataSize);

	return nl_cmd_handle(ifname, nl80211Command, cmdIdType, subCommand, vendorData, vendorDataSize, NULL, NULL);
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_driver_nl_detach(void)
 **************************************************************************
 *  \brief Resets/clear the Driver-NL interface
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_driver_nl_detach(void)
{
	int idx;

	console_printf("%s Entry\n", __FUNCTION__);

	if (dwpal_command_get_ended != (-1))
	{
		pid_t pid = getpid();
		char  socketName[SOCKET_NAME_LENGTH] = "\0";

		snprintf(socketName, sizeof(socketName) - 1, "%s_%d", COMMAND_ENDED_SOCKET, pid);

		if (close(dwpal_command_get_ended) == (-1))
		{
			console_printf("%s; close() fail; dwpal_command_get_ended= %d; errno= %d ('%s')\n", __FUNCTION__, dwpal_command_get_ended, errno, strerror(errno));
		}

		unlink(socketName);
		dwpal_command_get_ended = (-1);
	}

	if (interfaceIndexGet("Driver", "ALL", &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	/* dealocate the interface */
	free((void *)dwpalService[idx]);
	dwpalService[idx] = NULL;

	/* Cancel the listener thread, if it does exist */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CANCEL, NULL);
#endif
	threadSet(&listenerThreadId, THREAD_CANCEL, NULL);

	if (dwpal_driver_nl_detach(&context[idx]) == DWPAL_FAILURE)
	{
		console_printf("%s; dwpal_driver_nl_detach returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	if (isAnyInterfaceActive())
	{ /* There are still active interfaces */
		/* Create the listener thread, if it does NOT exist yet */
#if defined EVENT_CALLBACK_THREAD
		threadSet(&eventHandlerThreadId, THREAD_CREATE, eventHandlerThreadStart);
#endif
		threadSet(&listenerThreadId, THREAD_CREATE, listenerThreadStart);
	}
#if defined EVENT_CALLBACK_THREAD
	else if (dwpal_event_handler != (-1))
	{
		pid_t pid = getpid();
		char  socketName[SOCKET_NAME_LENGTH] = "\0";

		snprintf(socketName, sizeof(socketName) - 1, "%s_%d", EVENT_HANDLER_SOCKET, pid);

		if (close(dwpal_event_handler) == (-1))
		{
			console_printf("%s; close() fail; dwpal_event_handler= %d; errno= %d ('%s')\n", __FUNCTION__, dwpal_event_handler, errno, strerror(errno));
		}

		unlink(socketName);
		dwpal_event_handler = (-1);
	}
#endif

	return DWPAL_SUCCESS;
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_driver_nl_attach(DwpalExtNlEventCallback nlEventCallback)
 **************************************************************************
 *  \brief Driver-NL interface attach and event callback register
 *  \param[in] DwpalExtNlEventCallback nlEventCallback - The callback function to be called while event will be received via this interface
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_driver_nl_attach(DwpalExtNlEventCallback nlEventCallback,DwpalExtNlNonVendorEventCallback nlNonVendorEventCallback)
{
	int       idx;
	DWPAL_Ret ret;

	console_printf("%s Entry\n", __FUNCTION__);

	ret = interfaceIndexCreate("Driver", "ALL", &idx);
	if (ret == DWPAL_FAILURE)
	{
		console_printf("%s; interfaceIndexCreate returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}
	else if (ret == DWPAL_INTERFACE_ALREADY_UP)
	{
		console_printf("%s; Interface (idx= %d) is already up ==> cont...\n", __FUNCTION__, idx);
		return DWPAL_SUCCESS;
	}

	console_printf("%s; interfaceIndexCreate successfully; returned idx= %d\n", __FUNCTION__, idx);

	if (dwpal_socket_create(&dwpal_command_get_ended /*output*/, COMMAND_ENDED_SOCKET) == DWPAL_FAILURE)
	{
		console_printf("%s; dwpal_socket_create returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	/* Cancel the listener thread, if it does exist */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CANCEL, NULL);
#endif
	threadSet(&listenerThreadId, THREAD_CANCEL, NULL);

	if (dwpal_driver_nl_attach(&context[idx] /*OUT*/) == DWPAL_FAILURE)
	{
		console_printf("%s; dwpal_driver_nl_attach returned ERROR ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	/* nlEventCallback can be NULL */
	dwpalService[idx]->nlEventCallback = nlEventCallback;

	/* Register here the internal static callback function of the 'get command' event */
	dwpalService[idx]->nlCmdGetCallback = nlCmdGetCallback;


	dwpalService[idx]->nlNonVendorEventCallback = nlNonVendorEventCallback;

	/* Create the listener thread, if it does NOT exist yet */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CREATE, eventHandlerThreadStart);
#endif
	threadSet(&listenerThreadId, THREAD_CREATE, listenerThreadStart);

	return DWPAL_SUCCESS;
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_hostap_cmd_send(char *VAPName, char *cmdHeader, FieldsToCmdParse *fieldsToCmdParse, char *reply , size_t *replyLen)
 **************************************************************************
 *  \brief Build and send hostap command
 *  \param[in] char *VAPName - The interface's radio/VAP name to send the command to
 *  \param[in] char *cmdHeader - The beginning of the hostap command string
 *  \param[in] FieldsToCmdParse *fieldsToCmdParse - The command parsing information, in which accordingly, the command string (after the header) will be created
 *  \param[out] char *reply - The output string returning from the hostap command
 *  \param[in,out] size_t *replyLen - Provide the max output string length, and get back the actual string length
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_hostap_cmd_send(char *VAPName, char *cmdHeader, FieldsToCmdParse *fieldsToCmdParse, char *reply /*OUT*/, size_t *replyLen /*IN/OUT*/)
{
	int idx;

	if ( (VAPName == NULL) || (cmdHeader == NULL) || (reply == NULL) || (replyLen == NULL) )
	{
		console_printf("%s; input params error ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	console_printf("%s; VAPName= '%s', cmdHeader= '%s'\n", __FUNCTION__, VAPName, cmdHeader);

	if (interfaceIndexGet("hostap", VAPName, &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet (VAPName= '%s') returned ERROR ==> Abort!\n", __FUNCTION__, VAPName);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	if (context[idx] == NULL)
	{
		console_printf("%s; context[%d] is NULL ==> Abort!\n", __FUNCTION__, idx);
		return DWPAL_FAILURE;
	}

	if (dwpalService[idx]->isConnectionEstablishNeeded == true)
	{
		console_printf("%s; interface is being reconnected, but still NOT ready ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	if (dwpal_hostap_cmd_send(context[idx], cmdHeader, fieldsToCmdParse, reply, replyLen) == DWPAL_FAILURE)
	{
		console_printf("%s; '%s' command send error\n", __FUNCTION__, cmdHeader);
		return DWPAL_FAILURE;
	}

	cli_printf("%s; replyLen= %d\nreply=\n%s\n", __FUNCTION__, *replyLen, reply);

	return DWPAL_SUCCESS;
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_hostap_interface_detach(char *VAPName)
 **************************************************************************
 *  \brief Will reset and detach the interface towards/from the hostapd/supplicant of the requested radio/VAP interface
 *  \param[in] char *VAPName - The interface's radio/VAP name to detach from
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_hostap_interface_detach(char *VAPName)
{
	int idx;

	if (VAPName == NULL)
	{
		console_printf("%s; VAPName is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	console_printf("%s Entry; VAPName= '%s'\n", __FUNCTION__, VAPName);

	if (interfaceIndexGet("hostap", VAPName, &idx) == DWPAL_INTERFACE_IS_DOWN)
	{
		console_printf("%s; interfaceIndexGet (VAPName= '%s') returned ERROR ==> Abort!\n", __FUNCTION__, VAPName);
		return DWPAL_INTERFACE_IS_DOWN;
	}

	console_printf("%s; interfaceIndexGet returned idx= %d\n", __FUNCTION__, idx);

	/* dealocate the interface */
	free((void *)dwpalService[idx]);
	dwpalService[idx] = NULL;

	if (context[idx] == NULL)
	{
		console_printf("%s; context[%d] is NULL ==> Abort!\n", __FUNCTION__, idx);
		return DWPAL_FAILURE;
	}

	/* Cancel the listener thread, if it does exist */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CANCEL, NULL);
#endif
	threadSet(&listenerThreadId, THREAD_CANCEL, NULL);

	if (dwpal_hostap_interface_detach(&context[idx]) == DWPAL_FAILURE)
	{
		console_printf("%s; dwpal_hostap_interface_detach (VAPName= '%s') returned ERROR ==> Abort!\n", __FUNCTION__, VAPName);
		return DWPAL_FAILURE;
	}

	if (isAnyInterfaceActive())
	{ /* There are still active interfaces */
		/* Create the listener thread, if it does NOT exist yet */
#if defined EVENT_CALLBACK_THREAD
		threadSet(&eventHandlerThreadId, THREAD_CREATE, eventHandlerThreadStart);
#endif
		threadSet(&listenerThreadId, THREAD_CREATE, listenerThreadStart);
	}
#if defined EVENT_CALLBACK_THREAD
	else if (dwpal_event_handler != (-1))
	{
		pid_t pid = getpid();
		char  socketName[SOCKET_NAME_LENGTH] = "\0";

		snprintf(socketName, sizeof(socketName) - 1, "%s_%d", EVENT_HANDLER_SOCKET, pid);

		if (close(dwpal_event_handler) == (-1))
		{
			console_printf("%s; close() fail; dwpal_event_handler= %d; errno= %d ('%s')\n", __FUNCTION__, dwpal_event_handler, errno, strerror(errno));
		}

		unlink(socketName);
		dwpal_event_handler = (-1);
	}
#endif

	return DWPAL_SUCCESS;
}


/**************************************************************************/
/*! \fn DWPAL_Ret dwpal_ext_hostap_interface_attach(char *VAPName, DwpalExtHostapEventCallback hostapEventCallback)
 **************************************************************************
 *  \brief Hostapd/supplicant interface attach and event callback register
 *  \param[in] char *VAPName - The interface's radio/vap name to set attachment to
 *  \param[in] DwpalExtHostapEventCallback hostapEventCallback - The callback function to be called when an event will be received via this interface
 *  \return DWPAL_Ret (DWPAL_SUCCESS for success, other for failure)
 ***************************************************************************/
DWPAL_Ret dwpal_ext_hostap_interface_attach(char *VAPName, DwpalExtHostapEventCallback hostapEventCallback)
{
	int       idx;
	DWPAL_Ret ret;

	if (VAPName == NULL)
	{
		console_printf("%s; VAPName is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	if (hostapEventCallback == NULL)
	{
		console_printf("%s; hostapEventCallback is NULL ==> Abort!\n", __FUNCTION__);
		return DWPAL_FAILURE;
	}

	console_printf("%s Entry; VAPName= '%s'\n", __FUNCTION__, VAPName);

	ret = interfaceIndexCreate("hostap", VAPName, &idx);
	if (ret == DWPAL_FAILURE)
	{
		console_printf("%s; interfaceIndexCreate (VAPName= '%s') returned ERROR ==> Abort!\n", __FUNCTION__, VAPName);
		return DWPAL_FAILURE;
	}
	else if (ret == DWPAL_INTERFACE_ALREADY_UP)
	{
		console_printf("%s; Interface (idx= %d) is already up ==> cont...\n", __FUNCTION__, idx);
		return DWPAL_SUCCESS;
	}

	console_printf("%s; interfaceIndexCreate successfully; returned idx= %d\n", __FUNCTION__, idx);

#if defined EVENT_CALLBACK_THREAD
	if (dwpal_event_handler == -1)
	{
		if (dwpal_socket_create(&dwpal_event_handler /*output*/, EVENT_HANDLER_SOCKET) == DWPAL_FAILURE)
		{
			console_printf("%s; dwpal_socket_create returned ERROR ==> Abort!\n", __FUNCTION__);
			return DWPAL_FAILURE;
		}
	}
#endif

	/* Cancel the listener thread, if it does exist */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CANCEL, NULL);
#endif
	threadSet(&listenerThreadId, THREAD_CANCEL, NULL);

	if (dwpal_hostap_interface_attach(&context[idx] /*OUT*/, VAPName, NULL /*use one-way interface*/) == DWPAL_FAILURE)
	{
		console_printf("%s; dwpal_hostap_interface_attach (VAPName= '%s') returned ERROR ==> try later on...\n", __FUNCTION__, VAPName);

		/* in this case, continue and try to establish the connection later on */
		dwpalService[idx]->isConnectionEstablishNeeded = true;
	}
	else
	{
		dwpalService[idx]->isConnectionEstablishNeeded = false;
	}

	dwpalService[idx]->hostapEventCallback = hostapEventCallback;

	/* Create the listener thread, if it does NOT exist yet */
#if defined EVENT_CALLBACK_THREAD
	threadSet(&eventHandlerThreadId, THREAD_CREATE, eventHandlerThreadStart);
#endif
	threadSet(&listenerThreadId, THREAD_CREATE, listenerThreadStart);

	return DWPAL_SUCCESS;
}
