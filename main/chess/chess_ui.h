#ifndef CHESS_UI_H_
#define CHESS_UI_H_

#include <sdkconfig.h>

#ifdef CONFIG_CHESS_APP

// Run the chess app. Builds its own activity, takes over input, and blocks
// until the user selects Exit, restoring the caller's activity on return.
//
// Everything interesting lives in chess_game.c (the state machine) and
// chess_board.c (rendering); this unit is the Jade glue that pumps GUI events
// into the former and paints the latter.
void chess_ui_run(void);

#endif // CONFIG_CHESS_APP

#endif /* CHESS_UI_H_ */
