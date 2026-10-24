// shell.h - the Cardputer OS shell: a launcher menu and the active-app router,
// the single owner of the keyboard. Replaces the old standalone PTT task.
#pragma once

void shell_start(void);          // spawn the shell task (launcher + apps)
void shell_cue(const char *name);  // a status cue; painted only while an app that shows status is active
