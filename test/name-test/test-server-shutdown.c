/*
 * Copyright © 2018-2019 Ralf Habacker <ralf.habacker@freenet.de>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/**
 * This test checks whether a client can shutdown a dbus daemon
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "dbus/dbus-file.h"
#include "dbus/dbus-internals.h"
#include "dbus/dbus-sysdeps.h"
#include "dbus/dbus-test-tap.h"
#include "dbus/dbus-test.h"
#include "test/test-utils.h"
#include <dbus/dbus.h>

/* dbus_bus_get does not work yet */
static dbus_bool_t use_bus_get = FALSE;
static int add_wait_time = 0;
static HANDLE autolaunch_handle = NULL;

#define oom() _dbus_test_fatal ("Out of memory")

/**
 * helper function
 */
#define _dbus_error_set_from_message_with_location(a, b) \
  __dbus_error_set_from_message_with_location (__FILE__, __LINE__, \
                                               __FUNCTION__, a, b)

static void
__dbus_error_set_from_message_with_location (const char *file,
                                             int line,
                                             const char *function,
                                             DBusError *error,
                                             DBusMessage *message)
{
  char *str = NULL;
  dbus_message_get_args (message, NULL, DBUS_TYPE_STRING, &str,
                         DBUS_TYPE_INVALID);
  dbus_set_error (error, dbus_message_get_error_name (message),
                  "[%s(%d):%s] %s", file, line, function, str ? str : "");
}

static dbus_bool_t
_send_message (DBusConnection *conn,
               DBusError *error,
               int timeout,
               const char *interface,
               const char *method_str)
{
  DBusMessage *method;
  DBusMessage *reply;
  dbus_bool_t result = TRUE;

  dbus_error_init (error);

  method = dbus_message_new_method_call (DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
                                         interface, method_str);

  reply =
      dbus_connection_send_with_reply_and_block (conn, method, timeout, error);
  dbus_message_unref (method);
  if (reply == NULL)
    {
      result = FALSE;
      goto out;
    }

  if (autolaunch_handle != NULL)
    {
      DBusError error2;

      dbus_error_init (&error2);

      if (_dbus_win_event_wait (autolaunch_handle, 0, &error2))
        {
          _dbus_test_diag ("server already exited");
          result = FALSE;
          goto out;
        }
    }

  if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (strcmp (dbus_message_get_error_name (reply),
                  DBUS_ERROR_DISCONNECTED) == 0)
        {
          _dbus_error_set_from_message_with_location (error, reply);
          result = FALSE;
          goto out;
        }
      else
        {
          _dbus_error_set_from_message_with_location (error, reply);
          result = FALSE;
          goto out;
        }
    }
  result = TRUE;

out:
  _DBUS_ASSERT_ERROR_XOR_BOOL (error, result);
  if (reply)
    dbus_message_unref (reply);
  return result;
}

static dbus_bool_t
_server_check_connection (DBusConnection *conn, DBusError *error)
{
  if (use_bus_get)
    return _send_message (conn, error, -1, DBUS_INTERFACE_PEER,
                          "GetMachineId");
  else
    return _send_message (conn, error, -1, DBUS_INTERFACE_DBUS, "Hello");
}

static dbus_bool_t
_server_shutdown (DBusConnection *conn,
                  const char *scope,
                  int timeout,
                  DBusError *error)
{
  return _send_message (conn, error, timeout, DBUS_INTERFACE_EMBEDDED_TESTS,
                        "Shutdown");
}

typedef enum
{
  RUN_TEST_DEFAULT = 0,
  RUN_TEST_EXPECT_CONNECTION_TO_FAIL = 1,
  RUN_TEST_EXPECT_SERVER_API_TO_FAIL = 2,
  RUN_TEST_EXPECT_RESULT_TO_FAIL = 4,
  RUN_TEST_NO_EXPLICIT_SHUTDOWN = 8,
  RUN_TEST_SHUTDOWN_CANCELLED = 16,
} RunTestFlags;

static dbus_bool_t
check_results (DBusConnection *conn,
               DBusString *address,
               const char *scope,
               RunTestFlags flags,
               DBusError *error)
{
  if (add_wait_time)
    _dbus_sleep_milliseconds (add_wait_time);

  _dbus_test_diag ("%sconnection for address %s", conn ? "" : "no ",
                   _dbus_string_get_const_data (address));
  if (dbus_error_is_set (error))
    _dbus_test_diag ("Error is set: %s %s", error->name, error->message);

  if (dbus_error_is_set (error) && conn == NULL)
    {
      return flags & RUN_TEST_EXPECT_CONNECTION_TO_FAIL;
    }

  if (!dbus_error_is_set (error) && conn == NULL)
    {
      _dbus_test_fatal (
          "Failed to autolaunch session bus and no error was set");
    }

  if (add_wait_time)
    _dbus_sleep_milliseconds (add_wait_time);

  if (!_server_check_connection (conn, error))
    {
      return FALSE;
    }

  _dbus_test_diag ("client uses %s", _dbus_string_get_const_data (address));

  if (!(flags & RUN_TEST_NO_EXPLICIT_SHUTDOWN))
    {
      if (!_server_shutdown (conn, scope, -1, error))
        return FALSE;
      _dbus_test_diag ("server has been signaled to shut down");
    }
  else
    _dbus_test_diag ("server not been signaled to shut down, should exit by itself");

  return TRUE;
}

static dbus_bool_t
run_test (const char *scope, const char *test_data_dir, RunTestFlags flags)
{
  DBusConnection *conn = NULL;
  DBusError error;
  DBusString address;
  DBusString session_parameter;
  dbus_bool_t result = FALSE;

  dbus_error_init (&error);

  if (!_dbus_string_init (&address))
    oom ();

  _dbus_test_diag ("run test");

  if (*scope != '\0')
    {
      if (!_dbus_string_append_printf (&address, "autolaunch:scope=%s", scope))
        oom ();
    }
  else if (!_dbus_string_append_printf (&address, "autolaunch:"))
    oom ();

  if (!_dbus_string_init (&session_parameter))
    oom ();

  _dbus_test_check (strchr (test_data_dir, '"') == NULL);

  _dbus_test_check (strchr (_dbus_string_get_const_data (&address), '"') ==
                    NULL);

  if (!_dbus_string_append_printf (
          &session_parameter, "\"--config-file=%s/%s\" \"--address=%s\"",
          test_data_dir, "valid-config-files/listen-server-shutdown.conf",
          _dbus_string_get_const_data (&address)))
    oom ();

  if (flags & RUN_TEST_NO_EXPLICIT_SHUTDOWN)
    {
      if (!_dbus_string_append_printf (&session_parameter, " \"--auto-shutdown\""))
        oom ();
     }

  if (!_dbus_string_append_printf (&session_parameter, " \"--verbose\""))
    oom ();

  _dbus_test_diag ("session parameter '%s'",
                   _dbus_string_get_const_data (&session_parameter));

  _dbus_test_win_autolaunch_set_command_line_parameter (
      _dbus_string_get_const_data (&session_parameter));
  _dbus_test_win_set_autolaunch_handle_location (&autolaunch_handle);

  if (use_bus_get)
    {
      dbus_setenv ("DBUS_SESSION_BUS_ADDRESS",
                   _dbus_string_get_const_data (&address));
      _dbus_test_diag ("got env %s", getenv ("DBUS_SESSION_BUS_ADDRESS"));
      conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
      if (!conn)
        _dbus_test_fatal("couldn't access session bus");
      dbus_connection_set_exit_on_disconnect (conn, FALSE);
    }
  else
    conn = dbus_connection_open_private (
        _dbus_string_get_const_data (&address), &error);

  _dbus_test_diag ("After attempting to connect: autolaunch handle is %p",
                   autolaunch_handle);

  result = check_results (conn, &address, scope, flags, &error);

  if (conn)
    {
      dbus_connection_close (conn);
      dbus_connection_unref (conn);
    }

  if (result && (flags & RUN_TEST_NO_EXPLICIT_SHUTDOWN))
    {
      DBusConnection *conn2;
      DBusError error2;
      dbus_error_init (&error2);

      if (flags & RUN_TEST_SHUTDOWN_CANCELLED)
        {
          _dbus_test_diag ("opening second connection without delay");
        }
      else
        {
          _dbus_test_diag ("opening second connection with 100 ms delay");
          _dbus_sleep_milliseconds (100);
        }

      result = TRUE;

      conn2 = dbus_connection_open_private (
         _dbus_string_get_const_data (&address), &error2);
      if (conn2 == NULL)
        {
          _dbus_test_diag ("second connection failed: %s",
                           error2.message);
          result = FALSE;
        }
      else
        {
          _dbus_test_diag ("second connection is open");

          dbus_error_init (&error2);
          if (!_server_check_connection (conn2, &error2))
            {
              _dbus_test_diag ("checking second connection failed: %s",
                               error2.message);
              result = FALSE;
            }
          else
            {
              _dbus_test_diag ("checking second connection finished");
            }
          dbus_connection_close (conn2);
          dbus_connection_unref (conn2);
        }
        dbus_error_free (&error2);

        if (result == FALSE && flags & RUN_TEST_SHUTDOWN_CANCELLED)
          _dbus_test_fatal ("Cancelling auto shutdown is not correctly implemented");
    }

  _dbus_test_diag ("result = %d", result);

  if (autolaunch_handle != NULL)
    {
      if (_dbus_win_event_wait (autolaunch_handle, 5000, &error) == TRUE)
        _dbus_test_diag ("server has exited");
      else
        _dbus_test_fatal ("server did not auto-shutdown: got error '%s'", error.message);
    }
  else {
     _dbus_test_diag ("autolaunch handle is gone");
    }

  _dbus_string_free (&address);
  _dbus_string_free (&session_parameter);
  dbus_error_free (&error);

  return result;
}

static dbus_bool_t
run_test_okay (const char *scope, const char *test_data_dir)
{
  return run_test (scope, test_data_dir, RUN_TEST_DEFAULT);
}

#if 0
static dbus_bool_t
run_test_should_fail (const char *scope, const char *test_data_dir)
{
  return run_test (scope, test_data_dir, RUN_TEST_EXPECT_CONNECTION_TO_FAIL);
}
#endif

static dbus_bool_t
_dbus_shutdown_default_test (const char *test_data_dir)
{
  return run_test_okay ("", test_data_dir);
}

static dbus_bool_t
_dbus_auto_shutdown_test (const char *test_data_dir)
{
  return run_test ("", test_data_dir, RUN_TEST_NO_EXPLICIT_SHUTDOWN);
}

static dbus_bool_t
_dbus_auto_shutdown_test_cancelled_by_new_connection (const char *test_data_dir)
{
  return run_test ("", test_data_dir, RUN_TEST_NO_EXPLICIT_SHUTDOWN | RUN_TEST_SHUTDOWN_CANCELLED);
}

static DBusTestCase tests[] = { { "default", _dbus_shutdown_default_test },
                                { "auto-shutdown", _dbus_auto_shutdown_test },
                                { "auto-shutdown-cancelled", _dbus_auto_shutdown_test_cancelled_by_new_connection },
                                { NULL, NULL } };

int
main (int argc, char **argv)
{
  return _dbus_test_main (argc, argv, _DBUS_N_ELEMENTS (tests), tests,
                          /*DBUS_TEST_FLAGS_CHECK_MEMORY_LEAKS*/ 0, NULL,
                          NULL);
}
