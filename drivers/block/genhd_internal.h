#ifndef _GENHD_INTERNAL_H
#define _GENHD_INTERNAL_H

struct gendisk;

void scan_partitions(struct gendisk *disk);

#endif
