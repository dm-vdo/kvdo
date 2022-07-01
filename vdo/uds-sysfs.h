/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef UDS_SYSFS_H
#define UDS_SYSFS_H

/**
 * Called when the module is loaded to initialize the /sys/\<module_name\>
 * tree.
 *
 * @return 0 on success, or non-zero on error
 **/
int uds_init_sysfs(void);

/**
 * Called when the module is being unloaded to terminate the
 * /sys/\<module_name\> tree.
 **/
void uds_put_sysfs(void);

#endif  /* UDS_SYSFS_H */
