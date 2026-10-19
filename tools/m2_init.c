// m2_init.c - create the hidden PS5-compatible GPT on an internal M.2 SSD.
//
// Derived from ps5-linux/ps5-linux-tools m2_init.c (MIT). Extended with
// --device / --size-gb so the caller can target a specific device and cap the
// Linux partition to a user-chosen size instead of always filling the disk.
//
// The PS5 hypervisor watches LBA 0 on the M.2. Writing a normal GPT there is
// seen as a user partition table and the console may force a reformat. So the
// PMBR + GPT are *shifted* to LBA 65536 (PMBR), 65537 (primary header),
// 65538 (entry array), leaving a reserved tail before the alternate GPT.

#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SECTOR_SIZE 512
#define BYTES_PER_GB (1024ULL * 1024ULL * 1024ULL)
#define SECTORS_PER_GB (BYTES_PER_GB / SECTOR_SIZE)
#define PS5_M2_PART44_LBA_OFFSET 34224
#define PS5_M2_MBR_LBA 65536

#define MSDOS_MBR_SIGNATURE 0xaa55
#define EFI_PMBR_OSTYPE_EFI_GPT 0xEE

#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL
#define GPT_HEADER_REVISION_V1 0x00010000
#define GPT_PRIMARY_PARTITION_TABLE_LBA 1

#define GPT_ENTRY_NUMBERS 32
#define GPT_ENTRY_SIZE 128
#define GPT_ARRAY_SECTORS ((GPT_ENTRY_NUMBERS * GPT_ENTRY_SIZE) / SECTOR_SIZE)
#define GPT_RESERVED_SECTORS                                                   \
  (GPT_PRIMARY_PARTITION_TABLE_LBA + GPT_ARRAY_SECTORS)

#define UUID_SIZE 16

typedef struct {
  uint8_t b[UUID_SIZE];
} guid_t;

typedef guid_t efi_guid_t __attribute__((aligned(__alignof__(uint32_t))));

#define EFI_GUID(a, b, c, d...)                                                \
  (efi_guid_t) {                                                               \
    {                                                                          \
      (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff,   \
          (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, d      \
    }                                                                          \
  }

#define PARTITION_LINUX_FILE_SYSTEM_DATA_GUID                                  \
  EFI_GUID(0x0FC63DAF, 0x8483, 0x4772, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47,     \
           0x7D, 0xE4)

typedef struct {
  uint64_t required_to_function : 1;
  uint64_t reserved : 47;
  uint64_t type_guid_specific : 16;
} __attribute__((packed)) gpt_entry_attributes;

typedef struct {
  efi_guid_t partition_type_guid;
  efi_guid_t unique_partition_guid;
  uint64_t starting_lba;
  uint64_t ending_lba;
  gpt_entry_attributes attributes;
  uint16_t partition_name[72 / sizeof(uint16_t)];
} __attribute__((packed)) gpt_entry;

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc32;
  uint32_t reserved1;
  uint64_t my_lba;
  uint64_t alternate_lba;
  uint64_t first_usable_lba;
  uint64_t last_usable_lba;
  efi_guid_t disk_guid;
  uint64_t partition_entry_lba;
  uint32_t num_partition_entries;
  uint32_t sizeof_partition_entry;
  uint32_t partition_entry_array_crc32;
} __attribute__((packed)) gpt_header;

typedef struct {
  uint8_t boot_indicator;
  uint8_t start_head;
  uint8_t start_sector;
  uint8_t start_track;
  uint8_t os_type;
  uint8_t end_head;
  uint8_t end_sector;
  uint8_t end_track;
  uint32_t starting_lba;
  uint32_t size_in_lba;
} __attribute__((packed)) gpt_mbr_record;

typedef struct {
  uint8_t boot_code[440];
  uint32_t unique_mbr_signature;
  uint16_t unknown;
  gpt_mbr_record partition_record[4];
  uint16_t signature;
} __attribute__((packed)) legacy_mbr;

// Standard IEEE CRC-32 (poly 0xEDB88320), identical to zlib crc32(0, ...).
// Inlined so the tool links statically with no libz dependency.
static uint32_t crc32_ieee(uint32_t crc, const void *buf, size_t len) {
  static uint32_t table[256];
  static int have_table = 0;
  if (!have_table) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    have_table = 1;
  }
  const uint8_t *p = (const uint8_t *)buf;
  crc ^= 0xFFFFFFFFu;
  while (len--)
    crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [--device /dev/nvme0n1] [--size-gb N]\n"
          "  --device   target block device (default /dev/nvme0n1)\n"
          "  --size-gb  size of the Linux partition in GiB (default: whole "
          "usable disk)\n",
          argv0);
}

int main(int argc, char *argv[]) {
  const char *dev = "/dev/nvme0n1";
  uint64_t size_gb = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      dev = argv[++i];
    } else if (strcmp(argv[i], "--size-gb") == 0 && i + 1 < argc) {
      size_gb = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  int fd = open(dev, O_RDWR);
  if (fd == -1) {
    perror("open");
    return EXIT_FAILURE;
  }

  uint64_t bytes;
  if (ioctl(fd, BLKGETSIZE64, &bytes) == -1) {
    perror("ioctl");
    close(fd);
    return EXIT_FAILURE;
  }

  uint64_t total_sectors = bytes / SECTOR_SIZE;
  uint64_t lastlba = total_sectors - 1 - PS5_M2_PART44_LBA_OFFSET;
  uint64_t first_usable_lba = PS5_M2_MBR_LBA + GPT_RESERVED_SECTORS + 1;
  uint64_t last_usable_lba = lastlba - GPT_ARRAY_SECTORS - 1;
  uint64_t size_in_lba = lastlba - PS5_M2_MBR_LBA;

  printf("Disk Info:\n");
  printf("  Device: %s\n", dev);
  printf("  Total Bytes: %" PRIu64 "\n", bytes);
  printf("  Total Sectors: %" PRIu64 "\n", total_sectors);
  printf("  PS5 M2 Offset: %d\n", PS5_M2_PART44_LBA_OFFSET);
  printf("  PS5 MBR LBA: %d\n", PS5_M2_MBR_LBA);
  printf("  Calculated Last LBA: %" PRIu64 "\n", lastlba);
  printf("  First Usable LBA: %" PRIu64 "\n", first_usable_lba);
  printf("  Last Usable LBA: %" PRIu64 "\n", last_usable_lba);

  legacy_mbr mbr = {};
  mbr.signature = MSDOS_MBR_SIGNATURE;
  mbr.partition_record[0].os_type = EFI_PMBR_OSTYPE_EFI_GPT;
  mbr.partition_record[0].starting_lba =
      PS5_M2_MBR_LBA + GPT_PRIMARY_PARTITION_TABLE_LBA;
  mbr.partition_record[0].size_in_lba =
      (size_in_lba > 0xffffffff) ? 0xffffffff : (uint32_t)size_in_lba;

  printf("\nMBR Info:\n");
  printf("  MBR Start LBA: %u\n", mbr.partition_record[0].starting_lba);
  printf("  MBR Size (LBA): %u\n", mbr.partition_record[0].size_in_lba);

  uint8_t entries[SECTOR_SIZE * GPT_ARRAY_SECTORS] = {};
  gpt_entry *pte = (gpt_entry *)entries;

  uint64_t start_lba = ((first_usable_lba + 2047) / 2048) * 2048;
  uint64_t end_lba = ((last_usable_lba + 1) / 2048) * 2048 - 1;
  if (size_gb > 0) {
    uint64_t want = start_lba + size_gb * SECTORS_PER_GB - 1;
    want = ((want + 1) / 2048) * 2048 - 1;
    if (want < end_lba) {
      end_lba = want;
    }
  }

  pte[0].partition_type_guid = PARTITION_LINUX_FILE_SYSTEM_DATA_GUID;
  memset(pte[0].unique_partition_guid.b, 0x41, UUID_SIZE);
  pte[0].starting_lba = start_lba;
  pte[0].ending_lba = end_lba;

  printf("\nLinux partition:\n");
  printf("  Start LBA: %" PRIu64 "\n", start_lba);
  printf("  End LBA:   %" PRIu64 "\n", end_lba);
  printf("  Size:      %" PRIu64 " GiB\n",
         (uint64_t)(((end_lba - start_lba + 1) * SECTOR_SIZE) / BYTES_PER_GB));

  uint8_t gpt_sector[SECTOR_SIZE] = {};
  gpt_header *gpt = (gpt_header *)gpt_sector;
  gpt->signature = GPT_HEADER_SIGNATURE;
  gpt->revision = GPT_HEADER_REVISION_V1;
  gpt->header_size = sizeof(gpt_header);
  gpt->my_lba = PS5_M2_MBR_LBA + GPT_PRIMARY_PARTITION_TABLE_LBA;
  gpt->alternate_lba = lastlba;
  gpt->first_usable_lba = first_usable_lba;
  gpt->last_usable_lba = last_usable_lba;
  gpt->partition_entry_lba =
      PS5_M2_MBR_LBA + GPT_PRIMARY_PARTITION_TABLE_LBA + 1;
  gpt->num_partition_entries = GPT_ENTRY_NUMBERS;
  gpt->sizeof_partition_entry = GPT_ENTRY_SIZE;
  memset(gpt->disk_guid.b, 0x42, UUID_SIZE);

  gpt->partition_entry_array_crc32 = crc32_ieee(0, entries, sizeof(entries));
  gpt->header_crc32 = 0;
  gpt->header_crc32 = crc32_ieee(0, (uint8_t *)gpt, gpt->header_size);

  printf("\nPrimary GPT Header (LBA %" PRIu64 "):\n", gpt->my_lba);
  printf("  Signature: 0x%" PRIx64 "\n", gpt->signature);
  printf("  Header CRC32: 0x%08x\n", gpt->header_crc32);
  printf("  Partition Entry Array CRC32: 0x%08x\n",
         gpt->partition_entry_array_crc32);
  printf("  Partition Entry LBA: %" PRIu64 "\n", gpt->partition_entry_lba);

  if (lseek(fd, (off_t)PS5_M2_MBR_LBA * SECTOR_SIZE, SEEK_SET) == (off_t)-1) {
    perror("lseek");
    close(fd);
    return EXIT_FAILURE;
  }
  if (write(fd, &mbr, SECTOR_SIZE) != SECTOR_SIZE) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }
  if (write(fd, gpt_sector, SECTOR_SIZE) != SECTOR_SIZE) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }
  if (write(fd, entries, sizeof(entries)) != sizeof(entries)) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }

  gpt->my_lba = lastlba;
  gpt->alternate_lba = PS5_M2_MBR_LBA + GPT_PRIMARY_PARTITION_TABLE_LBA;
  gpt->partition_entry_lba = lastlba - GPT_ARRAY_SECTORS;
  gpt->header_crc32 = 0;
  gpt->header_crc32 = crc32_ieee(0, (uint8_t *)gpt, gpt->header_size);

  printf("\nAlternate GPT Header (LBA %" PRIu64 "):\n", gpt->my_lba);
  printf("  Header CRC32: 0x%08x\n", gpt->header_crc32);
  printf("  Partition Entry LBA: %" PRIu64 "\n", gpt->partition_entry_lba);

  if (lseek(fd, (off_t)(lastlba - GPT_ARRAY_SECTORS) * SECTOR_SIZE, SEEK_SET) ==
      (off_t)-1) {
    perror("lseek");
    close(fd);
    return EXIT_FAILURE;
  }
  if (write(fd, entries, sizeof(entries)) != sizeof(entries)) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }
  if (lseek(fd, (off_t)lastlba * SECTOR_SIZE, SEEK_SET) == (off_t)-1) {
    perror("lseek");
    close(fd);
    return EXIT_FAILURE;
  }
  if (write(fd, gpt_sector, SECTOR_SIZE) != SECTOR_SIZE) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }

  if (fsync(fd) == -1) {
    perror("fsync");
    close(fd);
    return EXIT_FAILURE;
  }

  printf("\nSuccessfully added Linux partition to %s.\n", dev);

  close(fd);
  return EXIT_SUCCESS;
}
