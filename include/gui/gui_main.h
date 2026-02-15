/**
 * @file gui_main.h
 * @brief Mercury HF Modem GUI - Public interface
 */

#ifndef GUI_MAIN_H_
#define GUI_MAIN_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GUI (create window, OpenGL context, ImGui)
 * @return 0 on success, -1 on failure
 */
int gui_init(void);

/**
 * @brief Run the GUI main loop (blocking)
 * @return 0 on normal exit
 */
int gui_main_loop(void);

/**
 * @brief Shutdown the GUI and cleanup resources
 */
void gui_shutdown(void);

/**
 * @brief GUI thread entry point (for use with pthread_create)
 * @param arg Unused
 * @return nullptr
 */
void* gui_thread_func(void* arg);

#ifdef __cplusplus
}
#endif

#endif // GUI_MAIN_H_
