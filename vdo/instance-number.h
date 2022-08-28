/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INSTANCE_NUMBER_H
#define INSTANCE_NUMBER_H

int vdo_allocate_instance(unsigned int *instance_ptr);

void vdo_release_instance(unsigned int instance);

void vdo_initialize_instance_number_tracking(void);

void vdo_clean_up_instance_number_tracking(void);

#endif /* INSTANCE_NUMBER_H */
