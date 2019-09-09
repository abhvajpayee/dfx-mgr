#ifndef _ACAPD_PRINT_H
#define _ACAPD_PRINT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DEBUG
#define acapd_debug(...)
#else
void acapd_debug(const char *format, ...);
#endif /* DEBUG */
void acapd_print(const char *format, ...);
void acapd_perror(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /*  _ACAPD_PRINT_H */
