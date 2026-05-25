#ifndef LATER_ACTION_H_
#define LATER_ACTION_H_

int action_create(const char *time_str);
int action_list(int verbose);
int action_show(const char *id_input);
int action_cancel(const char *id_input);
int action_delete(const char *id_input);
int action_logs(const char *id_input, int verbose);
int action_clean(void);

#endif // LATER_ACTION_H_
