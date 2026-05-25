#include "action.h"

#include "3rdparty/argparse/argparse.h"

#include <stdio.h>
#include <string.h>

static const char *const usages[] = {
    "later [<time>]",
    "later --list",
    "later --show <id>",
    "later --cancel <id>",
    "later --delete <id>",
    "later --logs <id>",
    "later --clean",
    NULL,
};

int main(int argc, const char *argv[])
{
    int list_flag = 0, clean_flag = 0, verbose_flag = 0, version_flag = 0;
    const char *show_id = NULL, *cancel_id = NULL, *delete_id = NULL;
    const char *logs_id = NULL;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_BOOLEAN('v', "version", &version_flag, "print version and exit", NULL, 0, 0),
        OPT_BOOLEAN('l', "list", &list_flag, "list all tasks", NULL, 0, 0),
        OPT_STRING('s', "show", &show_id, "show task details", NULL, 0, 0),
        OPT_STRING('L', "logs", &logs_id, "show task log output", NULL, 0, 0),
        OPT_STRING(0, "cancel", &cancel_id, "cancel a pending/running task", NULL, 0, 0),
        OPT_STRING(0, "delete", &delete_id, "delete a finished task", NULL, 0, 0),
        OPT_BOOLEAN(0, "clean", &clean_flag, "remove all finished tasks", NULL, 0, 0),
        OPT_BOOLEAN(0, "verbose", &verbose_flag, "show detailed output", NULL, 0, 0),
        OPT_END(),
    };

    struct argparse ap;
    argparse_init(&ap, options, usages, 0);
    argparse_describe(&ap, "\nlater — schedule shell commands for later execution", NULL);
    argc = argparse_parse(&ap, argc, argv);

    if (version_flag)
    {
        printf("later 0.1.0\n");
        return 0;
    }
    if (list_flag)
        return action_list(verbose_flag);
    if (show_id)
        return action_show(show_id);
    if (cancel_id)
        return action_cancel(cancel_id);
    if (delete_id)
        return action_delete(delete_id);
    if (logs_id)
        return action_logs(logs_id, verbose_flag);
    if (clean_flag)
        return action_clean();

    if (argc >= 1)
        return action_create(argv[0]);

    argparse_usage(&ap);
    return 0;
}
