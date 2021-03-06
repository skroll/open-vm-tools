/*********************************************************
 * Copyright (C) 2003 VMware, Inc. All rights reserved.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> // for access, crypt, etc.

#include "vmware.h"
#include "vm_version.h"
#include "codeset.h"
#include "posix.h"
#include "auth.h"
#include "str.h"
#include "log.h"

#ifdef USE_PAM
#   include "file.h"
#   include "config.h"
#   include "localconfig.h"
#   if defined __APPLE__
#      include <AvailabilityMacros.h>
#      if MAC_OS_X_VERSION_MAX_ALLOWED <= MAC_OS_X_VERSION_10_5
#         include <pam/pam_appl.h>
#      else
#         include <security/pam_appl.h>
#      endif
#   else
#      include <security/pam_appl.h>
#   endif
#   include <dlfcn.h>
#endif

#if defined(HAVE_CONFIG_H) || defined(sun)
#  include <crypt.h>
#endif

#define LOGLEVEL_MODULE auth
#include "loglevel_user.h"

#ifdef USE_PAM
#if defined(sun)
#define CURRENT_PAM_LIBRARY	"libpam.so.1"
#elif defined(__FreeBSD__)
#define CURRENT_PAM_LIBRARY	"libpam.so"
#elif defined(__APPLE__)
#define CURRENT_PAM_LIBRARY	"libpam.dylib"
#else
#define CURRENT_PAM_LIBRARY	"libpam.so.0"
#endif

static typeof(&pam_start) dlpam_start;
static typeof(&pam_end) dlpam_end;
static typeof(&pam_authenticate) dlpam_authenticate;
static typeof(&pam_setcred) dlpam_setcred;
static typeof(&pam_acct_mgmt) dlpam_acct_mgmt;
static typeof(&pam_strerror) dlpam_strerror;
#if 0  /* These three functions are not used yet */
static typeof(&pam_open_session) dlpam_open_session;
static typeof(&pam_close_session) dlpam_close_session;
static typeof(&pam_chauthtok) dlpam_chauthtok;
#endif

static struct {
   void       **procaddr;
   const char  *procname;
} authPAMImported[] = {
#define IMPORT_SYMBOL(x) { (void **)&dl##x, #x }
   IMPORT_SYMBOL(pam_start),
   IMPORT_SYMBOL(pam_end),
   IMPORT_SYMBOL(pam_authenticate),
   IMPORT_SYMBOL(pam_setcred),
   IMPORT_SYMBOL(pam_acct_mgmt),
   IMPORT_SYMBOL(pam_strerror),
#undef IMPORT_SYMBOL
};

static void *authPamLibraryHandle = NULL;


/*
 *----------------------------------------------------------------------
 *
 * AuthLoadPAM --
 *
 *      Attempt to load and initialize PAM library.
 *
 * Results:
 *      FALSE if load and/or initialization failed.
 *      TRUE  if initialization succeeded.
 *
 * Side effects:
 *      libpam loaded.  We never unload - some libpam modules use
 *      syslog() function, and glibc does not survive when arguments
 *      specified to openlog() are freeed from memory.
 *
 *----------------------------------------------------------------------
 */

static Bool
AuthLoadPAM(void)
{
   void *pam_library;
   int i;

   if (authPamLibraryHandle) {
      return TRUE;
   }
   pam_library = Posix_Dlopen(CURRENT_PAM_LIBRARY, RTLD_LAZY | RTLD_GLOBAL);
   if (!pam_library) {
#if defined(VMX86_TOOLS)
      /*
       * XXX do we even try to configure the pam libraries?
       * potential nightmare on all the possible guest OSes
       */

      Log("System PAM libraries are unusable: %s\n", dlerror());

      return FALSE;
#else
      char *liblocation;
      char *libdir;

      libdir = LocalConfig_GetPathName(DEFAULT_LIBDIRECTORY, CONFIG_VMWAREDIR);
      if (!libdir) {
         Log("System PAM library unusable and bundled one not found.\n");

         return FALSE;
      }
      liblocation = Str_SafeAsprintf(NULL, "%s/lib/%s/%s", libdir,
                                     CURRENT_PAM_LIBRARY, CURRENT_PAM_LIBRARY);
      free(libdir);

      pam_library = Posix_Dlopen(liblocation, RTLD_LAZY | RTLD_GLOBAL);
      if (!pam_library) {
         Log("Neither system nor bundled (%s) PAM libraries usable: %s\n",
             liblocation, dlerror());
         free(liblocation);

         return FALSE;
      }
      free(liblocation);
#endif
   }
   for (i = 0; i < ARRAYSIZE(authPAMImported); i++) {
      void *symbol = dlsym(pam_library, authPAMImported[i].procname);

      if (!symbol) {
         Log("PAM library does not contain required function: %s\n",
             dlerror());
         dlclose(pam_library);
         return FALSE;
      }

      *(authPAMImported[i].procaddr) = symbol;
   }

   authPamLibraryHandle = pam_library;
   Log("PAM up and running.\n");

   return TRUE;
}


static const char *PAM_username;
static const char *PAM_password;

#if defined(sun)
static int PAM_conv (int num_msg,                     // IN:
		     struct pam_message **msg,        // IN:
		     struct pam_response **resp,      // OUT:
		     void *appdata_ptr)               // IN:
#else
static int PAM_conv (int num_msg,                     // IN:
		     const struct pam_message **msg,  // IN:
		     struct pam_response **resp,      // OUT:
		     void *appdata_ptr)               // IN:
#endif
{
   int count;
   struct pam_response *reply = calloc(num_msg, sizeof(struct pam_response));

   if (!reply) {
      return PAM_CONV_ERR;
   }
   
   for (count = 0; count < num_msg; count++) {
      switch (msg[count]->msg_style) {
      case PAM_PROMPT_ECHO_ON:
         reply[count].resp_retcode = PAM_SUCCESS;
         reply[count].resp = PAM_username ? strdup(PAM_username) : NULL;
         /* PAM frees resp */
         break;
      case PAM_PROMPT_ECHO_OFF:
         reply[count].resp_retcode = PAM_SUCCESS;
         reply[count].resp = PAM_password ? strdup(PAM_password) : NULL;
         /* PAM frees resp */
         break;
      case PAM_TEXT_INFO:
         reply[count].resp_retcode = PAM_SUCCESS;
         reply[count].resp = NULL;
         /* ignore it... */
         break;
      case PAM_ERROR_MSG:
         reply[count].resp_retcode = PAM_SUCCESS;
         reply[count].resp = NULL;
         /* Must be an error of some sort... */
      default:
         while (--count >= 0) {
            free(reply[count].resp);
         }
         free(reply);

         return PAM_CONV_ERR;
      }
   }
   
   *resp = reply;

   return PAM_SUCCESS;
}

static struct pam_conv PAM_conversation = {
    &PAM_conv,
    NULL
};
#endif /* USE_PAM */


/*
 *----------------------------------------------------------------------
 *
 * Auth_AuthenticateUser --
 *
 *      Accept username/password And verfiy it
 *
 * Side effects:
 *      None.
 *
 * Results:
 *      
 *      The vmauthToken for the authenticated user, or NULL if
 *      authentication failed.
 *
 *----------------------------------------------------------------------
 */

AuthToken
Auth_AuthenticateUser(const char *user,  // IN:
                      const char *pass)  // IN:
{
   struct passwd *pwd;

#ifdef USE_PAM
   pam_handle_t *pamh;
   int pam_error;
#endif

   if (!CodeSet_Validate(user, strlen(user), "UTF-8")) {
      Log("User not in UTF-8\n");
      return NULL;
   }
   if (!CodeSet_Validate(pass, strlen(pass), "UTF-8")) {
      Log("Password not in UTF-8\n");                                                     
      return NULL;
   }

#ifdef USE_PAM
   if (!AuthLoadPAM()) {
      return NULL;
   }

   /*
    * XXX PAM can blow away our syslog level settings so we need
    * to call Log_InitEx() again before doing any more Log()s
    */

#define PAM_BAIL if (pam_error != PAM_SUCCESS) { \
                  Log_Error("%s:%d: PAM failure - %s (%d)\n", \
                            __FUNCTION__, __LINE__, \
                            dlpam_strerror(pamh, pam_error), pam_error); \
                  dlpam_end(pamh, pam_error); \
                  return NULL; \
                 }
   PAM_username = user;
   PAM_password = pass;

#if defined(VMX86_TOOLS)
   pam_error = dlpam_start("vmtoolsd", PAM_username, &PAM_conversation,
                           &pamh);
#else
   pam_error = dlpam_start("vmware-authd", PAM_username, &PAM_conversation,
                           &pamh);
#endif
   if (pam_error != PAM_SUCCESS) {
      Log("Failed to start PAM (error = %d).\n", pam_error);
      return NULL;
   }

   pam_error = dlpam_authenticate(pamh, 0);
   PAM_BAIL;
   pam_error = dlpam_acct_mgmt(pamh, 0);
   PAM_BAIL;
   pam_error = dlpam_setcred(pamh, PAM_ESTABLISH_CRED);
   PAM_BAIL;
   dlpam_end(pamh, PAM_SUCCESS);

   /* If this point is reached, the user has been authenticated. */
   setpwent();
   pwd = Posix_Getpwnam(user);
   endpwent();

#else /* !USE_PAM */

   /* All of the following issues are dealt with in the PAM configuration
      file, so put all authentication/priviledge checks before the
      corresponding #endif below. */
   
   setpwent(); //XXX can kill?
   pwd = Posix_Getpwnam(user);
   endpwent(); //XXX can kill?

   if (!pwd) {
      // No such user
      return NULL;
   }

   if (*pwd->pw_passwd != '\0') {
      char *namep = (char *) crypt(pass, pwd->pw_passwd);

      if (strcmp(namep, pwd->pw_passwd) != 0) {
         // Incorrect password
         return NULL;
      }

      // Clear out crypt()'s internal state, too.
      crypt("glurp", pwd->pw_passwd);
   }
#endif /* !USE_PAM */
   
   return pwd;
}

/*
 *----------------------------------------------------------------------
 *
 * Auth_CloseToken --
 *
 *      Do nothing.
 *
 * Side effects:
 *      None
 *
 * Results:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Auth_CloseToken(AuthToken token)  // IN:
{
}

