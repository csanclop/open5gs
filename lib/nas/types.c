/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ogs-nas.h"

int __ogs_nas_domain;

void *ogs_nas_from_plmn_id(ogs_nas_plmn_id_t *ogs_nas_plmn_id, ogs_plmn_id_t *plmn_id)
{
    memcpy(ogs_nas_plmn_id, plmn_id, OGS_PLMN_ID_LEN);
    if (plmn_id->mnc1 != 0xf)
    {
        ogs_nas_plmn_id->mnc1 = plmn_id->mnc1;
        ogs_nas_plmn_id->mnc2 = plmn_id->mnc2;
        ogs_nas_plmn_id->mnc3 = plmn_id->mnc3;
    }
    return ogs_nas_plmn_id;
}
void *ogs_nas_to_plmn_id(ogs_plmn_id_t *plmn_id, ogs_nas_plmn_id_t *ogs_nas_plmn_id)
{
    memcpy(plmn_id, ogs_nas_plmn_id, OGS_PLMN_ID_LEN);
    if (plmn_id->mnc1 != 0xf)
    {
        plmn_id->mnc1 = ogs_nas_plmn_id->mnc1;
        plmn_id->mnc2 = ogs_nas_plmn_id->mnc2;
        plmn_id->mnc3 = ogs_nas_plmn_id->mnc3;
    }
    return plmn_id;
}

static uint8_t nas_ambr_from_kbps(
    uint8_t *br, uint8_t *extended, uint8_t *extended2,
    uint64_t input)
{
    uint8_t length = 0;

    /* Octet 3 : 00000000 Reserved, 11111111 0kbps */
    if (input < 1) {
        *br = 0xff;
        length = ogs_max(length, 1);

        return length;
    }

    /*
     * Octet 7 : 00000000 
     * Use the value indicated by the APN-AMBR for downlink and
     *   APN-AMBR for downlink (extended) in octets 3 and 5.
     *
     * Octet 7 : 00000001 - 11111110
     * The APN-AMBR is
     *   (the binary coded value in 8 bits) * 256 Mbps +
     *      "the value indicated by the APN-AMBR for downlink
     *          and APN-AMBR for downlink (extended) in octets 3 and 5",
     *
     * giving a range of 256 Mbps to 65280 Mbps.  */
    if (input > (65200*1024)) {
        ogs_error("Overflow : %ldkbps > 65200Mbps", input);
        *extended2 = 0b11111110;
        length = ogs_max(length, 3);

        input %= (256*1024);
    }
    else if (input >= (256*1024) && input <= (65200*1024)) {
        *extended2 = input / (256*1024);
        length = ogs_max(length, 3);

        input %= (256*1024);
    }

    /* Octet 3 : 00000001 -  00111111 
     * The APN-AMBR is binary coded in 8 bits, using a granularity of 1 kbps
     * giving a range of values from 1 kbps to 63 kbps in 1 kbps increments. */
    if (input >= 1 && input <= 63) {
        *br = input;
        length = ogs_max(length, 1);
    }
    /* Octet 3 : 01000000 -  01111111 
     * The APN-AMBR is 
     *   64 kbps + ((the binary coded value in 8 bits –01000000) * 8 kbps) 
     * giving a range of values from 64 kbps to 568 kbps 
     *   in 8 kbps increments. */
    else if (input >= 64 && input <= 568) {
        *br = ((input - 64) / 8) + 0b01000000;
        length = ogs_max(length, 1);
    }
    /* Set to 568 Kbps */
    else if (input > 568 && input < 576) {
        *br = 0b01111111;
        length = ogs_max(length, 1);
    }
    /* Octet 3 : 10000000 -  111111110
     * The APN-AMBR is
     *   576 kbps + ((the binary coded value in 8 bits –10000000) * 64 kbps)
     * giving a range of values from 576 kbps to 8640 kbps
     *   in 64 kbps increments. */
    else if (input >= 576 && input <= 8640) {
        *br = ((input - 576) / 64) + 0b10000000;
        length = ogs_max(length, 1);
    }
    /* Set to 8640 Kbps */
    else if (input > 8640 && input < 8700) {
        *br = 0b11111110;
        length = ogs_max(length, 1);
    }

    /* If the network wants to indicate an APN-AMBR 
     * for downlink higher than 8640 kbps, 
     * it shall set octet 3 to "11111110", i.e. 8640 kbps, 
     * and shall encode the value for the APN-AMBR in octet 5. 
     *
     * Octet 5 : 00000000 
     * Use the value indicated by the APN-AMBR for downlink in octet 3.
     *
     * Octet 5 : All other values shall be interpreted as '11111010'.
     *
     * Octet 5 : 00000001 - 01001010 
     * The APN-AMBR is
     *   8600 kbps + ((the binary coded value in 8 bits) * 100 kbps),
     * giving a range of values from 8700 kbps to 16000 kbps
     *   in 100 kbps increments.
     */
    else if (input >= 8700 && input <= 16000) {
        *br = 0b11111110;
        *extended = ((input - 8600) / 100);
        length = ogs_max(length, 2);
    }
    /* Set to 16000 Kbps */
    else if (input > 16000 && input < (17*1024)) {
        *br = 0b11111110;
        *extended = 0b01001010;
        length = ogs_max(length, 2);
    }
    /* Octet 5: 01001011 - 10111010 
     * The APN-AMBR is
     *   16 Mbps + ((the binary coded value in 8 bits - 01001010) * 1 Mbps),
     * giving a range of values from 17 Mbps to 128 Mbps
     *   in 1 Mbps increments. */
    else if (input >= (17*1024) && input <= (128*1024)) {
        *br = 0b11111110;
        *extended = ((input - (16*1024)) / (1*1024)) + 0b01001010;
        length = ogs_max(length, 2);
    }
    /* Set to 128 Mbps */
    else if (input > (128*1024) && input < (130*1024)) {
        *br = 0b11111110;
        *extended = 0b10111010;
        length = ogs_max(length, 2);
    }
    /* Octet 5: 10111011 - 11111010
     * The APN-AMBR is
     *   128 Mbps + ((the binary coded value in 8 bits - 10111010) * 2 Mbps),
     * giving a range of values from 130 Mbps to 256 Mbps
     *   in 2 Mbps increments. */
    else if (input >= (130*1024) && input <= (256*1024)) {
        *br = 0b11111110;
        *extended = ((input - (128*1024)) / (2*1024)) + 0b10111010;
        length = ogs_max(length, 2);
    }

    return length;
}

static uint8_t nas_qos_from_kbps(
    uint8_t *br, uint8_t *extended, uint8_t *extended2,
    uint64_t input)
{
    uint8_t length = 0;

    /*
     * Octet 4
     * 11111111 0kbps
     */
    if (input < 1) {
        *br = 0xff;
        length = ogs_max(length, 1);

        return length;
    }

    /*
     * Octet 4
     *
     * In UE to network direction:
     * 00000000 Subscribed maximum bit rate
     *
     * In network to UE direction:
     * 00000000 Reserved
     *
     * Octet 4 : 00000001 - 00111111
     * giving a range of 1 kbps to 63 kbps in 1 kbps increments.
     *
     * Octet 4 : 01000000 - 01111111 
     * 64 kbps + (the binary coded value in 8 bits - 01000000) * 8 kbps
     * giving a range of 64 kbps to 568 kbps in 8 kbps increments.
     *
     * Octet 4 : 10000000 - 11111110
     * 576 kbps + (the binary coded value in 8 bits – 10000000) * 64 kbps
     * giving a range of 576 kbps to 8640 kbps in 64 kbps increments.
     */

    /* giving a range of 1 kbps to 63 kbps in 1 kbps increments. */
    if (input >= 1 && input <= 63) {
        *br = input;
        length = ogs_max(length, 1);
    }
    /* giving a range of 64 kbps to 568 kbps in 8 kbps increments. */
    else if (input >= 64 && input <= 568) {
        *br = ((input - 64) / 8) + 0b01000000;
        length = ogs_max(length, 1);
    }
    /* Set to 568 Kbps */
    else if (input > 568 && input < 576) {
        *br = 0b01111111;
        length = ogs_max(length, 1);
    }
    /* giving a range of 576 kbps to 8640 kbps in 64 kbps increments. */
    else if (input >= 576 && input <= 8640) {
        *br = ((input - 576) / 64) + 0b10000000;
        length = ogs_max(length, 1);
    }
    /* Set to 8640 Kbps */
    else if (input > 8640 && input < 8700) {
        *br = 0b11111110;
        length = ogs_max(length, 1);
    }

    /*
     * Octet 8
     * 00000000 Use the value indicated by the bit rate in octet 4
     *
     * Octet 8 : 00000001 - 01001010
     * 8600 kbps + (the binary coded value in 8 bits) * 100 kbps
     * giving a range of 8700 kbps to 16000 kbps in 100 kbps increments.
     *
     * Octet 8 : 01001011 - 10111010
     * 16 Mbps + (the binary coded value in 8 bits - 01001010) * 1 Mbps
     * giving a range of 17 Mbps to 128 Mbps in 1 Mbps increments.
     *
     * Octet 8 : 10111011 - 11111010
     * 128 Mbps + (the binary coded value in 8 bits - 10111010) * 2 Mbps
     * giving a range of 130 Mbps to 256 Mbps in 2 Mbps increments.
     */

    /* giving a range of 8700 kbps to 16000 kbps in 100 kbps increments. */
    else if (input >= 8700 && input <= 16000) {
        *br = 0b11111110;
        *extended = ((input - 8600) / 100);
        length = ogs_max(length, 2);
    }
    /* Set to 16000 Kbps */
    else if (input > 16000 && input < (17*1024)) {
        *br = 0b11111110;
        *extended = 0b01001010;
        length = ogs_max(length, 2);
    }
    /* giving a range of 17 Mbps to 128 Mbps in 1 Mbps increments. */
    else if (input >= (17*1024) && input <= (128*1024)) {
        *br = 0b11111110;
        *extended = ((input - (16*1024)) / (1*1024)) + 0b01001010;
        length = ogs_max(length, 2);
    }
    /* Set to 128 Mbps */
    else if (input > (128*1024) && input < (130*1024)) {
        *br = 0b11111110;
        *extended = 0b10111010;
        length = ogs_max(length, 2);
    }
    /* giving a range of 130 Mbps to 256 Mbps in 2 Mbps increments. */
    else if (input >= (130*1024) && input <= (256*1024)) {
        *br = 0b11111110;
        *extended = ((input - (128*1024)) / (2*1024)) + 0b10111010;
        length = ogs_max(length, 2);
    }
    /* Set to 256 Mbps */
    else if (input > (256*1024) && input < (260*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        length = ogs_max(length, 2);
    }

    /*
     * Octet 12 : 00000000
     * 00000000 Use the value indicated by the bit rate in octet 4 and 8
     *
     * Octet 12 : 00000001 - 00111101
     * 256Mbps + the binary coded value in 8 bits * 4Mbps
     * giving a range of 260 Mbps to 500 Mbps in 4 Mbps increments.
     *
     * Octet 12 : 00111110 - 10100001
     * 500Mbps + (the binary coded value in 8 bits - 00111101) * 10Mbps
     * giving a range of 510 Mbps to 1500 Mbps in 10 Mbps increments.
     *
     * Octet 12 : 10100010 - 11110110
     * 1500Mbps + (the binary coded value in 8 bits - 10100001) * 100Mbps
     * giving a range of 1600 Mbps to 10 Gbps Mbps in 100 Mbps increaments.
     */

    /* giving a range of values from 260M to 500M in 4M increments */
    else if (input >= (260*1024) && input <= (500*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = (input - (256*1024)) / (4*1024);
        length = ogs_max(length, 3);
    }
    /* if a range of values from 500M to 510M */
    else if (input > (500*1024) && input < (510*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = 0b00111101;
        length = ogs_max(length, 3);
    }
    /* giving a range of values from 510M to 1500M in 10M increments */
    else if (input >= (510*1024) && input <= (1500*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = (input - (500*1024)) / (10*1024) + 0b00111101;
        length = ogs_max(length, 3);
    }
    /* if a range of values from 1500M to 1600M */
    else if (input > (1500*1024) && input < (1600*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = 0b10100001;
        length = ogs_max(length, 3);
    }
    /* giving a range of values from 1600M to 10000M in 100M increments */
    else if (input >= (1600*1024) && input <= (10*1000*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = (input - (1500*1024)) / (100*1024) + 0b10100001;
        length = ogs_max(length, 3);
    }
    /* if the sending entity want to indicate BR higher than 10000Mbps */
    else if (input > (10*1000*1024)) {
        *br = 0b11111110;
        *extended = 0b11111010;
        *extended2 = 0b11110110;
        length = ogs_max(length, 3);
    }

    return length;
}

void apn_ambr_build(
    ogs_nas_apn_aggregate_maximum_bit_rate_t *apn_aggregate_maximum_bit_rate,
    uint32_t dl_apn_ambr, uint32_t ul_apn_ambr)
{
    uint8_t length = 0;

    dl_apn_ambr = dl_apn_ambr / 1024; /* Kbps */
    ul_apn_ambr = ul_apn_ambr / 1024; /* Kbps */

    memset(apn_aggregate_maximum_bit_rate, 0,
        sizeof(ogs_nas_apn_aggregate_maximum_bit_rate_t));

    length = ogs_max(length, nas_ambr_from_kbps(
                &apn_aggregate_maximum_bit_rate->dl_apn_ambr,
                &apn_aggregate_maximum_bit_rate->dl_apn_ambr_extended,
                &apn_aggregate_maximum_bit_rate->dl_apn_ambr_extended2,
                dl_apn_ambr));

    length = ogs_max(length, nas_ambr_from_kbps(
                &apn_aggregate_maximum_bit_rate->ul_apn_ambr,
                &apn_aggregate_maximum_bit_rate->ul_apn_ambr_extended,
                &apn_aggregate_maximum_bit_rate->ul_apn_ambr_extended2,
                ul_apn_ambr));

    apn_aggregate_maximum_bit_rate->length = length*2;
}

void eps_qos_build(ogs_nas_eps_quality_of_service_t *eps_qos, uint8_t qci,
    uint64_t dl_mbr, uint64_t ul_mbr, uint64_t dl_gbr, uint64_t ul_gbr)
{
    uint8_t length = 0;

    dl_mbr = dl_mbr / 1024; /* Kbps */
    ul_mbr = ul_mbr / 1024; /* Kbps */
    dl_gbr = dl_gbr / 1024; /* Kbps */
    ul_gbr = ul_gbr / 1024; /* Kbps */

    memset(eps_qos, 0, sizeof(ogs_nas_eps_quality_of_service_t));

    eps_qos->qci = qci;

    if (dl_mbr)
        length = ogs_max(length, nas_qos_from_kbps(
                    &eps_qos->dl_mbr,
                    &eps_qos->dl_mbr_extended,
                    &eps_qos->dl_mbr_extended2,
                    dl_mbr));

    if (ul_mbr)
        length = ogs_max(length, nas_qos_from_kbps(
                    &eps_qos->ul_mbr,
                    &eps_qos->ul_mbr_extended,
                    &eps_qos->ul_mbr_extended2,
                    ul_mbr));

    if (dl_gbr)
        length = ogs_max(length, nas_qos_from_kbps(
                    &eps_qos->dl_gbr,
                    &eps_qos->dl_gbr_extended,
                    &eps_qos->dl_gbr_extended2,
                    dl_gbr));

    if (ul_gbr)
        length = ogs_max(length, nas_qos_from_kbps(
                    &eps_qos->ul_gbr,
                    &eps_qos->ul_gbr_extended,
                    &eps_qos->ul_gbr_extended2,
                    ul_gbr));

    eps_qos->length = length*4+1;
}

void ogs_nas_tai_list_build(
        ogs_nas_tracking_area_identity_list_t *target,
        tai0_list_t *source0, tai2_list_t *source2)
{
    int i = 0, j = 0, size = 0;

    tai0_list_t target0;
    tai2_list_t target2;
    ogs_nas_plmn_id_t ogs_nas_plmn_id;

    ogs_assert(target);
    ogs_assert(source0);
    ogs_assert(source2);

    memset(target, 0, sizeof(ogs_nas_tracking_area_identity_list_t));
    memset(&target0, 0, sizeof(tai0_list_t));
    memset(&target2, 0, sizeof(tai2_list_t));

    for (i = 0; source0->tai[i].num; i++)
    {
        ogs_assert(source0->tai[i].type == TAI0_TYPE);
        target0.tai[i].type = source0->tai[i].type;

        /* <Spec> target->num = source->num - 1 */
        ogs_assert(source0->tai[i].num < OGS_MAX_NUM_OF_TAI);
        target0.tai[i].num = source0->tai[i].num - 1;
        memcpy(&target0.tai[i].plmn_id,
                ogs_nas_from_plmn_id(&ogs_nas_plmn_id, &source0->tai[i].plmn_id),
                OGS_PLMN_ID_LEN);

        for (j = 0; j < source0->tai[i].num; j++) 
        {
            target0.tai[i].tac[j] = htons(source0->tai[i].tac[j]);
        }

        size = (1 + 3 + 2 * source0->tai[i].num);
        if ((target->length + size) > OGS_NAS_MAX_TAI_LIST_LEN)
        {
            ogs_warn("Overflow: Ignore remained TAI LIST(length:%d, size:%d)",
                    target->length, size);
            return;
        }
        memcpy(target->buffer + target->length, &target0.tai[i], size);
        target->length += size;
    }

    if (source2->num)
    {
        memset(&target2, 0, sizeof(target2));

        ogs_assert(source2->type == TAI1_TYPE || source2->type == TAI2_TYPE);
        target2.type = source2->type;

        /* <Spec> target->num = source->num - 1 */
        ogs_assert(source2->num < OGS_MAX_NUM_OF_TAI);
        target2.num = source2->num - 1;

        size = (1 + (3 + 2) * source2->num);
        if ((target->length + size) > OGS_NAS_MAX_TAI_LIST_LEN)
        {
            ogs_warn("Overflow: Ignore remained TAI LIST(length:%d, size:%d)",
                    target->length, size);
            return;
        }
        for (i = 0; i < source2->num; i++) 
        {
            memcpy(&target2.tai[i].plmn_id,
                    ogs_nas_from_plmn_id(&ogs_nas_plmn_id, &source2->tai[i].plmn_id),
                    OGS_PLMN_ID_LEN);
            target2.tai[i].tac = htons(source2->tai[i].tac);
        }
        memcpy(target->buffer + target->length, &target2, size);
        target->length += size;
    }
}
