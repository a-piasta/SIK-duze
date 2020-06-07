#ifndef _RADIO_TELNET_H_
#define _RADIO_TELNET_H_

/* telnet commands list
 * from https://www.ibm.com/support/knowledgecenter/SSLTBW_2.4.0/com.ibm.zos.v2r4.hald001/telcmds.htm#telcmds__rfc854
 */

#define SE    "\xf0"
#define NOP   "\xf1"
#define DM    "\xf2"
#define BRK   "\xf3"
#define IP    "\xf4"
#define AO    "\xf5"
#define AYT   "\xf6"
#define EC    "\xf7"
#define EL    "\xf8"
#define GA    "\xf9"
#define SB    "\xfa"
#define WILL  "\xfb"
#define WONT  "\xfc"
#define DO    "\xfd"
#define DONT  "\xfe"
#define IAC   "\xff"

#define ECHO      "\x1"
#define LINEMODE  "\x22"

#define MOVE_LEFT_UP   "\x1b[H"
#define CLRSCR         "\x1b[2J"
#define ENDL           "\r\n"
#define UNDERSCORE     "\x1b[4m"
#define NO_ATTR        "\x1b[0m"
#define ERASE_LINE     "\x1b[K"

#define MOVE_LEFT_UP_LEN  3
#define CLRSCR_LEN        4
#define ENDL_LEN          2
#define UNDERSCORE_LEN    4
#define NO_ATTR_LEN       4
#define ERASE_LINE_LEN    3

#define CHANGE_MODE       (IAC DO LINEMODE IAC WILL ECHO)
#define CHANGE_MODE_LEN   6

#define UP    0
#define DOWN  1
#define CRLF  2

const char *message[] = {
  "\x1b\x5b\x41",
  "\x1b\x5b\x42",
  "\xd\x0"
};

const ssize_t msg_len[] = {3, 3, 2};

const char *szukaj = "Szukaj pośrednika";
const char *posrednik = "Pośrednik ";
const char *koniec = "Koniec";

#define SZUKAJ_LEN      18
#define POSREDNIK_LEN   11
#define KONIEC_LEN      6

#endif  // _RADIO_TELNET_H_
