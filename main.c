#define _XOPEN_SOURCE 700
#include "partition-manager.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Keep the installed application native and allow an explicitly restricted device view. */
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_set_prgname("partition-manager"); g_set_application_name("Partition Manager");
    g_setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", TRUE);
    char *device=NULL; gboolean readonly=FALSE;
    GOptionEntry options[]={
        {"device", 0, 0, G_OPTION_ARG_FILENAME, &device, "Show only the specified device or disk image", "PATH"},
        {"read-only", 0, 0, G_OPTION_ARG_NONE, &readonly, "Allow planning but disable applying changes", NULL},
        {NULL}
    };
    GOptionContext *context=g_option_context_new("— native partition manager");
    g_option_context_add_main_entries(context, options, NULL); GError *error=NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error))
    { g_printerr("%s\n", error->message); g_error_free(error); g_option_context_free(context); return 1; }
    g_option_context_free(context);
    if (device)
    {
        char *canonical=realpath(device, NULL);
        if (!canonical) { g_printerr("Cannot resolve device: %s\n", device); g_free(device); return 1; }
        g_free(device); device=canonical;
    }
    int status=ui_run(argc, argv, device, readonly || geteuid()!=0);
    g_free(device); return status;
}
