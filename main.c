#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <errno.h>

#include "utils.h"
#include "rs232.h"

#include "protocol.h"

enum {
    ERR_NO = 0,
    ERR_OVRFLW,
    ERR_MSG,
    ERR_CRC,
    ERR_STATUS,
    ERR_DATA,
    ERR_TIMEOUT
};

static const char *__err_str[] = {
    "Buffer margins error",
    "Wrong message or format",
    "CRC checking error",
    "Status code signals of error",
    "Data error",
    "Timeout"
};

uint32_t data_crc;

static void __usage(const char *str);

int __send_data(int fd, size_t *data_sz);
int __check_crc(unsigned int as, unsigned int ae);
void __end_session(void);

static int __handshake_reply(uint8_t *buf, size_t n);
static int __status_reply(uint8_t *buf, size_t n);
static int __crc_reply(uint8_t *buf, size_t n);

typedef struct __bl_proto
{
    int cmd;
    int (*req)(const uint8_t *buf, size_t n);
    int (*reply)(uint8_t *buf, size_t n);
} bl_proto_t;

bl_proto_t proto_parse[] = {
    {BL_PROTO_CMD_HANDSHAKE,  NULL, __handshake_reply},
    {BL_PROTO_CMD_ERASE,      NULL, NULL},
    {BL_PROTO_CMD_FLASH,      NULL, NULL},
    {BL_PROTO_CMD_FLASH_DATA, NULL, __status_reply},
    {BL_PROTO_CMD_EOS,        NULL, __status_reply},
    {BL_PROTO_CMD_DATA_CRC,   NULL, __crc_reply},
    {-1, NULL, NULL}
};

int send_msg(int proto_cmd, uint8_t *data, size_t sz, unsigned int timeout)
{
    int ret;
    unsigned int timed;
    uint8_t buf[256];
    bl_proto_t *ptr = proto_parse;

    if (sz > 256)
        return ERR_OVRFLW;

    for (ptr = proto_parse; ptr->cmd != -1; ptr++)
        if (ptr->cmd == proto_cmd)
            break;

    /* Unknown command */
    if (ptr->cmd == -1)
        return 1;
    /* See if we have special case for request */
    if (ptr->req) {
        ret = ptr->req(data, sz);
        if (ret < 0)
            return ret;
    } else {
        buf[0] = proto_cmd;
        memcpy(buf + 1, data, sz);
        buf[sz + 1] = BL_PROTO_EOM;
        buf[sz + 2] = crc8(buf, sz + 2);
        ret = sz + 3;
    }

    ret = rs232_send(buf, ret);
    if (ret < 0)
        return ret;
    /* If timeout is specified, wait for reply with ~timeout */
    if (timeout) {
        for (timed = 0; timed < timeout * 1000; timed += 1000) {
            ret = rs232_poll(buf, 4096);
            if (ret == -EAGAIN || !ret)
                continue;
            else if (ret < 0)
                return ret;

            if (ptr->reply) {
                return ptr->reply(buf, ret);
            } else
                return 0;
        }
    } else {
        ret = rs232_poll(buf, 4096);
        if (ret < 0)
            return ret;
        if (ptr->reply)
            return ptr->reply(buf, ret);
    }

    return ERR_TIMEOUT;
}

int main(int argc, char *argv[])
{
    int ret, fd;
    int bdrate = B115200;
    size_t data_sz;

    if (argc < 3) {
        __usage(argv[0]);
        return 1;
    }

    printf("Opening serial device '%s'\n", argv[1]);
    ret = rs232_open(argv[1], bdrate);
    if(ret) {
        printf("Can't open serial device: %s\n", strerror(-ret));
        return EXIT_FAILURE;
    }

    ret = send_msg(BL_PROTO_CMD_HANDSHAKE, NULL, 0, 1000);
    if (ret < 0) {
        printf("Error sending handshake request: %s\n", strerror(-ret));
        return EXIT_FAILURE;
    } else if (ret != 0) {
        printf("Handshake failed: %s\n", __err_str[ret - 1]);
        return EXIT_FAILURE;
    }

    printf("Connection with bootloader established.\n");

    /* Getting info on a file to flash */
    fd = open(argv[2], O_RDONLY);
    if (fd == -1) {
        printf("Error opening binary file: %s\n", strerror(errno));
        __end_session();
        return EXIT_FAILURE;
    }

    ret = __send_data(fd, &data_sz);
    if (ret) {
        printf("Failed sending program to STM32 microcontroller: %d\n", ret);
        __end_session();
        return EXIT_FAILURE;
    }

    ret = __check_crc(0, data_sz);
    if (ret) {
        printf("Failed checking of flashed data: %d\n", ret);
        __end_session();
        return EXIT_FAILURE;
    }

    __end_session();

    return EXIT_SUCCESS;

#if 0
    n = rs232_poll(buf, 4096);
    if (n != 9) {
        printf("Failed handshaking with STM32.\n");
        return EXIT_FAILURE;
    } else if (crc8(buf, 8) != buf[8]) {
        printf("CRC mismatch in handshake.\n");
        return EXIT_FAILURE;
    }

    printf("Protocol version: %d.%d; Board ID: %02X:%02X:%02X:%02X.\n",
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    printf("Flashable area: %d\n", buf[6]);

    ret = __bl_erase(1);
    if (ret < 0) {
        printf("Failed sending erase request: %s\n", strerror(-ret));
        return EXIT_FAILURE;
    }
    for (i = 0; i < 10; i++) {
        sleep(1);
        n = rs232_poll(buf, 4096);
        if (n == -EAGAIN)
            continue;

        if (n == 3) {
            if (crc8(buf, 2) != buf[2]) {
                printf("CRC error in erase response.\n");
                return EXIT_FAILURE;
            } else {
                printf("Erase command finished: %d\n", buf[0]);
                break;
            }
        }
    }

    if (i == 10)
        printf("Timeout waiting for erase response.\n");

    uint8_t data[512];
    data[0] = 1; data[1] = 2; data[2] = 3; data[3] = 4;
    __bl_flash(data, 4);
    for (i = 0; i < 10; i++) {
        sleep(1);
        n = rs232_poll(buf, 4096);
        if (n == -EAGAIN)
            continue;

        if (n == 3) {
            if (crc8(buf, 2) != buf[2]) {
                printf("CRC error in flash response.\n");
                return EXIT_FAILURE;
            } else {
                printf("Flash command finished: %d\n", buf[0]);
                break;
            }
        }
    }

    if (i == 10)
        printf("Timeout waiting for flash response.\n");

    __bl_data(data, 4);
    __end_session();
#endif
}

static void __usage(const char *str)
{
    printf("%s: [serial_device]\n", str);
}

#if 0

static int __bl_erase(unsigned int sector)
{
    uint8_t msg[] = {BL_PROTO_CMD_ERASE, sector, BL_PROTO_EOM, 0};

    msg[3] = crc8(msg, 3);

    return rs232_send(msg, 4);;
}

static int __bl_flash(const uint8_t *buf, size_t size)
{
    uint8_t msg[] = {BL_PROTO_CMD_FLASH, 0, 0};

    msg[3] = crc8(msg, 2);

    return rs232_send(msg, 3);
}

static int __bl_data(const uint8_t *buf, size_t size)
{
    return rs232_send(buf, size);
}

static int __bl_handshake(void)
{
    uint8_t msg[] = {BL_PROTO_CMD_HANDSHAKE, BL_PROTO_EOM, 0};
    msg[2] = crc8(msg, 2);
    return rs232_send(msg, 3);
}

static int __end_session(void)
{
    uint8_t msg[] = {BL_PROTO_CMD_EOS, BL_PROTO_EOM, 0};
    msg[2] = crc8(msg, 2);

    return rs232_send(msg, 3);
}

#endif

void __end_session(void)
{
    int ret;

    ret = send_msg(BL_PROTO_CMD_EOS, NULL, 0, 10000);
    if (ret < 0) {
        printf("Error waiting for session end: %s\n", strerror(-ret));
        exit(EXIT_FAILURE);
    } else if (ret != 0) {
        printf("End of session status error: %d\n", ret);
        exit(EXIT_FAILURE);
    }

    printf("Session ended.\n");
}

int __send_data(int fd, size_t *data_sz)
{
    size_t sz;
    uint32_t crc;
    uint8_t data[256];
    int ret, tr;
    uint8_t len;

    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (sz % 4)
        return ERR_DATA;

    *data_sz = sz;

    while (sz) {
        tr = (sz >= 256) ? 256 : sz;
        ret = read(fd, data, tr);
        if (ret != tr)
            return ERR_DATA;

        len = tr - 1;
        ret = send_msg(BL_PROTO_CMD_FLASH_DATA, &len, 1, 1000000);
        if (ret) {
            return ret;
        }

        ret = rs232_send(data, tr);
        if (ret < 0)
            return ret;

        crc = crc32(data, tr, crc);
        sz -= tr;
    }

    data_crc = crc;

    return 0;
}

int __check_crc(unsigned int as, unsigned int ae)
{
    uint8_t msg[] = {
        as & 0xFF, (as>>8) & 0xFF, (as>>16) & 0xFF, (as>>24) & 0xFF,
        ae & 0xFF, (ae>>8) & 0xFF, (ae>>16) & 0xFF, (ae>>24) & 0xFF
    };
    return send_msg(BL_PROTO_CMD_DATA_CRC, msg, 8, 1000000);
}

static int __handshake_reply(uint8_t *buf, size_t n)
{
    if (n != 9)
        return ERR_MSG;
    else if (crc8(buf, 8) != buf[8])
        return ERR_CRC;

    printf("Protocol ver %d.%d\n"
           "BoardID: %02X:%02X:%02X:%02X\n"
           "Flashable area: %d\n",
           buf[0], buf[1],
           buf[2], buf[3], buf[4], buf[5],
           buf[6]);

    return 0;
}

static int __status_reply(uint8_t *buf, size_t n)
{
    if (n != 3)
        return ERR_MSG;
    else if (crc8(buf, 2) != buf[2])
        return ERR_CRC;

    if (buf[0] != BL_PROTO_STATUS_OK) {
        return ERR_STATUS;
    }

    return 0;
}

static int __crc_reply(uint8_t *buf, size_t n)
{
    union {
        uint32_t crc;
        uint8_t b[4];
    } crc_val;

    if (n != 6)
        return ERR_MSG;
    else if (crc8(buf, 5) != buf[5])
        return ERR_CRC;

    crc_val.b[0] = buf[0];
    crc_val.b[1] = buf[1];
    crc_val.b[2] = buf[2];
    crc_val.b[3] = buf[3];

    printf("CRC %X:%X \n", data_crc, crc_val.crc);

    return 0;
}
