/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/gloria/src/uds/zone.h#1 $
 */

#ifndef ZONE_H
#define ZONE_H

enum {
  MAX_ZONES = 16,
};

/**
 * Return the number of zones.
 *
 * @return  the number of zones
 **/
unsigned int getZoneCount(void) __attribute__((warn_unused_result));

/**
 * Set the number of zones.
 *
 * Must be called prior to initializeLocalIndexQueues() to have any effect.
 *
 * @param count   Desired number of zones. If set to 0,
 *                default behavior will apply.
 *
 * @return UDS_SUCCESS or an error code
 **/
int setZoneCount(unsigned int count);

/**
 * Reset the number of zones back to 0.
 *
 * This should only be used for testing to allow a set of tests to change
 * the zone count multiple times.
 *
 **/
void resetZoneCount(void);

#endif /* ZONE_H */
