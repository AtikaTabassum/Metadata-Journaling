#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BLOCK_SIZE 4096

/* Layout (must match mkfs + validator) */
#define JOURNAL_BLOCK_IDX 1
#define JOURNAL_BLOCKS    16
#define INODE_BMAP_IDX    17
#define DATA_BMAP_IDX     18
#define INODE_START_IDX   19
#define DATA_START_IDX    21

#define DIRECT_POINTERS 8

#define JOURNAL_MAGIC "JNL\0"

/* Journal record types */
#define RECORD_DATA   1
#define RECORD_COMMIT 2

struct journal_header {
    char magic[4];
    uint32_t head;
};

struct record_header {
    uint32_t type;
    uint32_t size;
};

struct data_record {
    struct record_header h;
    uint32_t home_block;
    uint8_t block[BLOCK_SIZE];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void read_block(int fd, uint32_t block, void *buf) {
    if (pread(fd, buf, BLOCK_SIZE, (off_t)block * BLOCK_SIZE) != BLOCK_SIZE)
        die("pread");
}

static void write_block(int fd, uint32_t block, void *buf) {
    if (pwrite(fd, buf, BLOCK_SIZE, (off_t)block * BLOCK_SIZE) != BLOCK_SIZE)
        die("pwrite");
}

/* ---------- Journal helpers ---------- */

static void journal_init(int fd, struct journal_header *jh) {
    uint8_t buf[BLOCK_SIZE];
    read_block(fd, JOURNAL_BLOCK_IDX, buf);
    memcpy(jh, buf, sizeof(*jh));

    if (memcmp(jh->magic, JOURNAL_MAGIC, 4) != 0) {
        memset(buf, 0, BLOCK_SIZE);
        memcpy(jh->magic, JOURNAL_MAGIC, 4);
        jh->head = sizeof(*jh);
        memcpy(buf, jh, sizeof(*jh));
        write_block(fd, JOURNAL_BLOCK_IDX, buf);
    }
}

static int journal_has_space(struct journal_header *jh, uint32_t bytes) {
    return jh->head + bytes <= JOURNAL_BLOCKS * BLOCK_SIZE;
}

static void journal_write(int fd, struct journal_header *jh,
                          const void *data, uint32_t size) {
    if (pwrite(fd, data, size,
               (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + jh->head) != (ssize_t)size)
        die("journal write");

    jh->head += size;

    uint8_t buf[BLOCK_SIZE];
    read_block(fd, JOURNAL_BLOCK_IDX, buf);
    memcpy(buf, jh, sizeof(*jh));
    write_block(fd, JOURNAL_BLOCK_IDX, buf);
}

static void journal_append_data(int fd, struct journal_header *jh,
                                uint32_t home_block, void *block) {
    struct data_record rec;
    rec.h.type = RECORD_DATA;
    rec.h.size = sizeof(rec.h) + sizeof(uint32_t) + BLOCK_SIZE;
    rec.home_block = home_block;
    memcpy(rec.block, block, BLOCK_SIZE);
    journal_write(fd, jh, &rec, rec.h.size);
}

static void journal_append_commit(int fd, struct journal_header *jh) {
    struct record_header h;
    h.type = RECORD_COMMIT;
    h.size = sizeof(h);
    journal_write(fd, jh, &h, h.size);
}

/* ---------- Bitmap helpers ---------- */

static int bitmap_test(uint8_t *b, uint32_t i) {
    return (b[i / 8] >> (i % 8)) & 1;
}

static void bitmap_set(uint8_t *b, uint32_t i) {
    b[i / 8] |= (1 << (i % 8));
}

/* ---------- MAIN ---------- */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: ./journal <create|install>\n");
        return 1;
    }

    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) die("open");

    /* ================= CREATE ================= */
    if (strcmp(argv[1], "create") == 0) {
        struct journal_header jh;
        journal_init(fd, &jh);

        uint8_t inode_bitmap[BLOCK_SIZE];
        uint8_t inode_block[BLOCK_SIZE];
        uint8_t dir_block[BLOCK_SIZE];

        read_block(fd, INODE_BMAP_IDX, inode_bitmap);
        read_block(fd, INODE_START_IDX, inode_block);
        read_block(fd, DATA_START_IDX, dir_block);

        /* find free inode */
        uint32_t new_inum = 0;
        while (bitmap_test(inode_bitmap, new_inum))
            new_inum++;

        bitmap_set(inode_bitmap, new_inum);

        struct inode *inodes = (struct inode *)inode_block;
        struct inode *ino = &inodes[new_inum];
        memset(ino, 0, sizeof(*ino));

        ino->type = 1; /* file */
        ino->links = 1;
        ino->size = 0;
        ino->ctime = ino->mtime = (uint32_t)time(NULL);

        struct dirent *ents = (struct dirent *)dir_block;
        for (int i = 0; i < BLOCK_SIZE / sizeof(struct dirent); i++) {
            if (ents[i].inode == 0 && ents[i].name[0] == '\0') {
                ents[i].inode = new_inum;
                strcpy(ents[i].name, "file");
                break;
            }
        }

        uint32_t need =
            3 * (sizeof(struct record_header) + 4 + BLOCK_SIZE) +
            sizeof(struct record_header);

        if (!journal_has_space(&jh, need)) {
            printf("Journal full, run install first\n");
            close(fd);
            return 1;
        }

        journal_append_data(fd, &jh, INODE_BMAP_IDX, inode_bitmap);
        journal_append_data(fd, &jh, INODE_START_IDX, inode_block);
        journal_append_data(fd, &jh, DATA_START_IDX, dir_block);
        journal_append_commit(fd, &jh);

        printf("Metadata journaled\n");
    }

    /* ================= INSTALL ================= */
    else if (strcmp(argv[1], "install") == 0) {
        struct journal_header jh;
        uint8_t buf[BLOCK_SIZE];

        read_block(fd, JOURNAL_BLOCK_IDX, buf);
        memcpy(&jh, buf, sizeof(jh));

        if (memcmp(jh.magic, JOURNAL_MAGIC, 4) != 0 || jh.head == 0) {
            printf("No journal to install\n");
            close(fd);
            return 1;
        }

        uint32_t off = sizeof(struct journal_header);
        struct data_record pending[16];
        int count = 0;

        while (off < jh.head) {
            struct record_header h;
            pread(fd, &h, sizeof(h),
                  (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + off);

            if (h.type == RECORD_DATA) {
                pread(fd, &pending[count], h.size,
                      (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + off);
                count++;
            } else if (h.type == RECORD_COMMIT) {
                for (int i = 0; i < count; i++)
                    write_block(fd,
                                pending[i].home_block,
                                pending[i].block);
                count = 0;
            }
            off += h.size;
        }

        /* clear journal */
        memset(buf, 0, BLOCK_SIZE);
        memcpy(buf, JOURNAL_MAGIC, 4);
        write_block(fd, JOURNAL_BLOCK_IDX, buf);

        printf("Journal installed and cleared\n");
    }

    else {
        fprintf(stderr, "Unknown command\n");
    }

    close(fd);
    return 0;
}
