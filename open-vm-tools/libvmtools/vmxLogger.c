/*********************************************************
 * Copyright (C) 2010 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file vmxLogger.c
 *
 * A logger that writes the logs to the VMX log file.
 */

#include "vmtoolsInt.h"
#include "vmware/tools/guestrpc.h"

typedef struct VMXLoggerData {
   GlibLogger     handler;
#if GLIB_CHECK_VERSION(2,32,0)
   GMutex         lock;
#else
   GStaticMutex   lock;
#endif
   RpcChannel    *chan;
} VMXLoggerData;

static inline void
VMXLoggerLock(VMXLoggerData *self)
{
#if GLIB_CHECK_VERSION(2,32,0)
   g_mutex_lock(&self->lock);
#else
   g_static_mutex_lock(&self->lock);
#endif
}

static inline void
VMXLoggerUnlock(VMXLoggerData *self)
{
#if GLIB_CHECK_VERSION(2,32,0)
   g_mutex_unlock(&self->lock);
#else
   g_static_mutex_unlock(&self->lock);
#endif
}


/*
 *******************************************************************************
 * VMXLoggerLog --                                                        */ /**
 *
 * Logs a message to the VMX using the backdoor.
 *
 * The logger uses its own RpcChannel, opening and closing the channel for each
 * log message sent. This is not optimal, especially if the application already
 * has an RpcChannel instantiated; this could be fixed by providing a way for
 * the application to provide its own RpcChannel to the logging code, if it uses
 * one, so that this logger can re-use it.
 *
 * @param[in] domain    Unused.
 * @param[in] level     Log level.
 * @param[in] message   Message to log.
 * @param[in] data      VMX logger data.
 *
 *******************************************************************************
 */

static void
VMXLoggerLog(const gchar *domain,
             GLogLevelFlags level,
             const gchar *message,
             gpointer data)
{
   VMXLoggerData *logger = data;

   VMXLoggerLock(logger);

   if (RpcChannel_Start(logger->chan)) {
      gchar *msg;
      gint cnt = VMToolsAsprintf(&msg, "log %s", message);

      /*
       * XXX: RpcChannel_Send() can log stuff in certain situations, which will
       * cause this to blow up. Hopefully we won't hit those too often since
       * we're stopping / starting the channel for each log message.
       */
      RpcChannel_Send(logger->chan, msg, cnt, NULL, NULL);

      g_free(msg);
      RpcChannel_Stop(logger->chan);
   }

   VMXLoggerUnlock(logger);
}


/*
 *******************************************************************************
 * VMXLoggerDestroy --                                                    */ /**
 *
 * Cleans up the internal state of a VMX logger.
 *
 * @param[in] data   VMX logger data.
 *
 *******************************************************************************
 */

static void
VMXLoggerDestroy(gpointer data)
{
   VMXLoggerData *logger = data;
   RpcChannel_Destroy(logger->chan);

#if GLIB_CHECK_VERSION(2,32,0)
   g_mutex_clear(&logger->lock);
#else
   g_static_mutex_free(&logger->lock);
#endif
   g_free(logger);
}


/*
 *******************************************************************************
 * VMToolsCreateVMXLogger --                                              */ /**
 *
 * Configures a new VMX logger.
 *
 * @return The VMX logger data.
 *
 *******************************************************************************
 */

GlibLogger *
VMToolsCreateVMXLogger(void)
{
   VMXLoggerData *data = g_new0(VMXLoggerData, 1);
   data->handler.logfn = VMXLoggerLog;
   data->handler.addsTimestamp = TRUE;
   data->handler.shared = TRUE;
   data->handler.dtor = VMXLoggerDestroy;

#if GLIB_CHECK_VERSION(2,32,0)
   g_mutex_init(&data->lock);
#else
   g_static_mutex_init(&data->lock);
#endif
   data->chan = BackdoorChannel_New();
   return &data->handler;
}

