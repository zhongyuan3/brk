#ifndef BRK_GENHD_INTERNAL_H
#define BRK_GENHD_INTERNAL_H

struct gendisk;

void scan_partitions(struct gendisk *disk);

#endif
