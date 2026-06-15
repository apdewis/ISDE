#ifndef _CHILD_H
#define _CHILD_H

#include "session.h"


Child *child_spawn(Session *s, const char *command, int respawn, int is_wm);
void   child_reap(Session *s);
void   child_kill_all(Session *s);
Child *child_find_pid(Session *s, pid_t pid);
void   child_remove(Session *s, Child *c);
void   restart_ui_children(Session *s);
void   child_process_pending_respawns(Session *s);
long long child_next_respawn_deadline(Session *s);

#endif