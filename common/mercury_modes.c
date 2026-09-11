/* Startup/listen mode table (index -> FreeDV mode).
 *
 * Its own translation unit so that anything needing the index<->mode mapping
 * can link it without dragging in mercury_cli.c's audioio/radio_io/ldpc
 * dependencies.  That matters for the control-port tests, which assert on the
 * index space the MODE command accepts: duplicating the table in the test
 * would let the two drift apart and silently invalidate those assertions.
 *
 * The index space is the user-facing one: `-m <index>`, `-l` to list, and the
 * MODE control-port command all speak it, so entries must only ever be
 * appended -- reordering would silently repoint every operator's config.
 */

#include "freedv_api.h"
#include "mercury_cli.h"

int freedv_modes[] = { FREEDV_MODE_DATAC1,
                       FREEDV_MODE_DATAC3,
                       FREEDV_MODE_DATAC0,
                       FREEDV_MODE_DATAC4,
                       FREEDV_MODE_DATAC13,
                       FREEDV_MODE_DATAC14,
                       FREEDV_MODE_FSK_LDPC,
                       FREEDV_MODE_DATAC15,
                       FREEDV_MODE_DATAC16,
                       FREEDV_MODE_DATAC17,
                       FREEDV_MODE_QAM16C2 };

char *freedv_mode_names[] = { "DATAC1",
                              "DATAC3",
                              "DATAC0",
                              "DATAC4",
                              "DATAC13",
                              "DATAC14",
                              "FSK_LDPC",
                              "DATAC15",
                              "DATAC16",
                              "DATAC17",
                              "QAM16C2" };

int mercury_cli_mode_count(void)
{
    return (int)(sizeof(freedv_modes) / sizeof(freedv_modes[0]));
}
