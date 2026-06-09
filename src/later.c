#include "action.h"

#include "3rdparty/argparse/argparse.h"

#include <stdio.h>
#include <string.h>

static const char *const usages[] = {
    "later [<time>]   schedule piped or typed commands (e.g. 17:30, +30m, +2h, +1d)",
    "later <option>   inspect or manage tasks",
    NULL};

int main(int argc, const char *argv[])
{
    int version_flag = 0;
    int list_flag = 0;
    int clean_flag = 0;
    int purge_flag = 0;
    int verbose_flag = 0;

    const char *show_id = NULL;
    const char *cancel_id = NULL;
    const char *pause_id = NULL;
    const char *resume_id = NULL;
    const char *delete_id = NULL;
    const char *log_id = NULL;
    const char *retry_id = NULL;

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_BOOLEAN('l', "list", &list_flag, "list all tasks", NULL, 0, 0),
        OPT_STRING('s', "show", &show_id, "show task details", NULL, 0, 0),
        OPT_STRING('L', "log", &log_id, "show task log output", NULL, 0, 0),
        OPT_BOOLEAN(0, "version", &version_flag, "print version and exit", NULL, 0, 0),
        OPT_STRING(0, "cancel", &cancel_id, "cancel a pending/running task", NULL, 0, 0),
        OPT_STRING(0, "pause", &pause_id, "pause a pending/running task", NULL, 0, 0),
        OPT_STRING(0, "resume", &resume_id, "resume a paused task", NULL, 0, 0),
        OPT_STRING(0, "delete", &delete_id, "delete a finished task", NULL, 0, 0),
        OPT_STRING(0, "retry", &retry_id, "rerun an existing task's commands", NULL, 0, 0),
        OPT_BOOLEAN(0, "clean", &clean_flag, "remove all finished tasks", NULL, 0, 0),
        OPT_BOOLEAN(0, "purge", &purge_flag, "cancel all tasks and erase the data dir", NULL, 0, 0),
        OPT_BOOLEAN(0, "verbose", &verbose_flag, "show detailed output", NULL, 0, 0),
        OPT_END()};

    struct argparse ap;
    argparse_init(&ap, options, usages, 0);
    argparse_describe(&ap, "\nlater - schedule commands for later execution", NULL);
    argc = argparse_parse(&ap, argc, argv);

    if (version_flag)
    {
        printf("later 0.2.0\n");
        return 0;
    }
    if (list_flag)
        return action_list(verbose_flag);
    if (show_id)
        return action_show(show_id);
    if (cancel_id)
        return action_cancel(cancel_id);
    if (pause_id)
        return action_pause(pause_id);
    if (resume_id)
        return action_resume(resume_id);
    if (delete_id)
        return action_delete(delete_id);
    if (log_id)
        return action_log(log_id, verbose_flag);
    if (clean_flag)
        return action_clean();
    if (purge_flag)
        return action_purge();
    if (retry_id)
        return action_retry(retry_id, argc >= 1 ? argv[0] : NULL);

    if (argc >= 1)
        return action_create(argv[0]);

    argparse_usage(&ap);

    return 0;
}
