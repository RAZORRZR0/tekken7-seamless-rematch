// Included by imgui.h via -DIMGUI_USER_CONFIG. Failed ImGui asserts are logged to rematch_mod.log
// instead of calling abort(), which would kill the game.
#pragma once
void ImAssertLog(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ImAssertLog(#_EXPR, __FILE__, __LINE__))
