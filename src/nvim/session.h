#ifndef NVIM_SESSIONS_H
#define NVIM_SESSIONS_H

#include "nvim/api/private/defs.h"


Dict get_ui_layout(void);

void session_save_to_file(Dict* p_ui, const char* fname);

// void session_save_to_file(Dict);
Dict session_restore(void);

#endif 
