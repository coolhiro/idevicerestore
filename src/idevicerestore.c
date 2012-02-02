/*
 * idevicerestore.c
 * Restore device firmware and filesystem
 *
 * Copyright (c) 2010 Joshua Hill. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <plist/plist.h>

#include "dfu.h"
#include "tss.h"
#include "img3.h"
#include "ipsw.h"
#include "common.h"
#include "normal.h"
#include "restore.h"
#include "recovery.h"
#include "idevicerestore.h"

int use_apple_server;

static struct option longopts[] = {
	{ "uuid",    required_argument, NULL, 'u' },
	{ "debug",   no_argument,       NULL, 'd' },
	{ "help",    no_argument,       NULL, 'h' },
	{ "erase",   no_argument,       NULL, 'e' },
	{ "custom",  no_argument,       NULL, 'c' },
	{ "cydia",   no_argument,       NULL, 's' },
	{ "exclude", no_argument,       NULL, 'x' },
	{ NULL, 0, NULL, 0 }
};

void usage(int argc, char* argv[]) {
	char* name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS] FILE\n", (name ? name + 1 : argv[0]));
	printf("Restore/upgrade IPSW firmware FILE to an iPhone/iPod Touch.\n");
	printf("  -u, --uuid UUID\ttarget specific device by its 40-digit device UUID\n");
	printf("  -d, --debug\t\tenable communication debugging\n");
	printf("  -h, --help\t\tprints usage information\n");
	printf("  -e, --erase\t\tperform a full restore, erasing all data\n");
	printf("  -c, --custom\t\trestore with a custom firmware\n");
	printf("  -s, --cydia\t\tuse Cydia's signature service instead of Apple's\n");
	printf("  -x, --exclude\t\texclude nor/baseband upgrade\n");
	printf("\n");
}

int main(int argc, char* argv[]) {
	int opt = 0;
	int optindex = 0;
	char* ipsw = NULL;
	char* uuid = NULL;
	int tss_enabled = 0;
	use_apple_server=1;

	// create an instance of our context
	struct idevicerestore_client_t* client = (struct idevicerestore_client_t*) malloc(sizeof(struct idevicerestore_client_t));
	if (client == NULL) {
		error("ERROR: Out of memory\n");
		return -1;
	}
	memset(client, '\0', sizeof(struct idevicerestore_client_t));

	while ((opt = getopt_long(argc, argv, "dhcesxu:", longopts, &optindex)) > 0) {
		switch (opt) {
		case 'h':
			usage(argc, argv);
			return 0;

		case 'd':
			client->flags |= FLAG_DEBUG;
			idevicerestore_debug = 1;
			break;

		case 'e':
			client->flags |= FLAG_ERASE;
			break;

		case 'c':
			client->flags |= FLAG_CUSTOM;
			break;

		case 's':
			use_apple_server=0;

		case 'x':
			client->flags |= FLAG_EXCLUDE;
			break;

		case 'u':
			uuid = optarg;
			break;

		default:
			usage(argc, argv);
			return -1;
		}
	}

	if ((argc-optind) == 1) {
		argc -= optind;
		argv += optind;

		ipsw = argv[0];
	} else {
		usage(argc, argv);
		return -1;
	}

	if (client->flags & FLAG_DEBUG) {
		idevice_set_debug_level(1);
		irecv_set_debug_level(1);
	}

	client->uuid = uuid;
	client->ipsw = ipsw;

	// check which mode the device is currently in so we know where to start
	if (check_mode(client) < 0 || client->mode->index == MODE_UNKNOWN) {
		error("ERROR: Unable to discover device mode. Please make sure a device is attached.\n");
		return -1;
	}
	info("Found device in %s mode\n", client->mode->string);

	// discover the device type
	if (check_device(client) < 0 || client->device->index == DEVICE_UNKNOWN) {
		error("ERROR: Unable to discover device type\n");
		return -1;
	}
	info("Identified device as %s\n", client->device->product);

	if (client->mode->index == MODE_RESTORE) {
		if (restore_reboot(client) < 0) {
			error("ERROR: Unable to exit restore mode\n");
			return -1;
		}
	}

	// extract buildmanifest
	plist_t buildmanifest = NULL;
	if (client->flags & FLAG_CUSTOM) {
		info("Extracting Restore.plist from IPSW\n");
		if (ipsw_extract_restore_plist(ipsw, &buildmanifest) < 0) {
			error("ERROR: Unable to extract Restore.plist from %s\n", ipsw);
			return -1;
		}
	} else {
		info("Extracting BuildManifest from IPSW\n");
		if (ipsw_extract_build_manifest(ipsw, &buildmanifest, &tss_enabled) < 0) {
			error("ERROR: Unable to extract BuildManifest from %s\n", ipsw);
			return -1;
		}
	}

	/* check if device type is supported by the given build manifest */
	if (build_manifest_check_compatibility(buildmanifest, client->device->product) < 0) {
		error("ERROR: could not make sure this firmware is suitable for the current device. refusing to continue.\n");
		return -1;
	}

	/* print iOS information from the manifest */
	build_manifest_get_version_information(buildmanifest, &client->version, &client->build);

	info("Product Version: %s\n", client->version);
	info("Product Build: %s\n", client->build);

	if (client->flags & FLAG_CUSTOM) {
		/* prevent signing custom firmware */
		tss_enabled = 0;
		info("Custom firmware requested. Disabled TSS request.\n");
	}

	// choose whether this is an upgrade or a restore (default to upgrade)
	client->tss = NULL;
	plist_t build_identity = NULL;
	if (client->flags & FLAG_CUSTOM) {
		build_identity = plist_new_dict();
		{
			plist_t node;
			plist_t comp;
			plist_t info;
			plist_t manifest;

			info = plist_new_dict();
			plist_dict_insert_item(info, "RestoreBehavior", plist_new_string((client->flags & FLAG_ERASE) ? "Erase" : "Update"));
			plist_dict_insert_item(info, "Variant", plist_new_string((client->flags & FLAG_ERASE) ? "Customer Erase Install (IPSW)" : "Customer Upgrade Install (IPSW)"));
			plist_dict_insert_item(build_identity, "Info", info);

			manifest = plist_new_dict();

			char tmpstr[256];
			char p_all_flash[128];
			char lcmodel[8];
			strcpy(lcmodel, client->device->model);
			int x = 0;
			while (lcmodel[x]) {
				lcmodel[x] = tolower(lcmodel[x]);
				x++;
			}

			sprintf(p_all_flash, "Firmware/all_flash/all_flash.%s.%s", lcmodel, "production");
			strcpy(tmpstr, p_all_flash);
			strcat(tmpstr, "/manifest");

			// get all_flash file manifest
			char *files[16];
			char *fmanifest = NULL;
			uint32_t msize = 0;
			if (ipsw_extract_to_memory(ipsw, tmpstr, &fmanifest, &msize) < 0) {
				error("ERROR: could not extract %s from IPSW\n", tmpstr);
				return -1;
			}

			char *tok = strtok(fmanifest, "\r\n");
			int fc = 0;
			while (tok) {
				files[fc++] = strdup(tok);
				if (fc >= 16) {
					break;
				}
				tok = strtok(NULL, "\r\n");
			}
			free(fmanifest);

			for (x = 0; x < fc; x++) {
				info = plist_new_dict();
				strcpy(tmpstr, p_all_flash);
				strcat(tmpstr, "/");
				strcat(tmpstr, files[x]);
				plist_dict_insert_item(info, "Path", plist_new_string(tmpstr));
				comp = plist_new_dict();
				plist_dict_insert_item(comp, "Info", info);
				const char* compname = get_component_name(files[x]);
				if (compname) {
					plist_dict_insert_item(manifest, compname, comp);
					if (!strncmp(files[x], "DeviceTree", 10)) {
						plist_dict_insert_item(manifest, "RestoreDeviceTree", plist_copy(comp));
					}
				} else {
					error("WARNING: unhandled component %s\n", files[x]);
					plist_free(comp);
				}
				free(files[x]);
				files[x] = NULL;
			}

			// add iBSS
			sprintf(tmpstr, "Firmware/dfu/iBSS.%s.%s.dfu", lcmodel, "RELEASE");
			info = plist_new_dict();
			plist_dict_insert_item(info, "Path", plist_new_string(tmpstr));
			comp = plist_new_dict();
			plist_dict_insert_item(comp, "Info", info);
			plist_dict_insert_item(manifest, "iBSS", comp);

			// add iBEC
			sprintf(tmpstr, "Firmware/dfu/iBEC.%s.%s.dfu", lcmodel, "RELEASE");
			info = plist_new_dict();
			plist_dict_insert_item(info, "Path", plist_new_string(tmpstr));
			comp = plist_new_dict();
			plist_dict_insert_item(comp, "Info", info);
			plist_dict_insert_item(manifest, "iBEC", comp);

			// add kernel cache
			node = plist_dict_get_item(buildmanifest, "KernelCachesByTarget");
			if (node && (plist_get_node_type(node) == PLIST_DICT)) {
				char tt[4];
				strncpy(tt, lcmodel, 3);
				tt[3] = 0;
				plist_t kdict = plist_dict_get_item(node, tt);
				if (kdict && (plist_get_node_type(kdict) == PLIST_DICT)) {
					plist_t kc = plist_dict_get_item(kdict, "Release");
					if (kc && (plist_get_node_type(kc) == PLIST_STRING)) {
						info = plist_new_dict();
						plist_dict_insert_item(info, "Path", plist_copy(kc));
						comp = plist_new_dict();
						plist_dict_insert_item(comp, "Info", info);
						plist_dict_insert_item(manifest, "KernelCache", comp);
						plist_dict_insert_item(manifest, "RestoreKernelCache", plist_copy(comp));

					}
				}
			}

			// add ramdisk
			node = plist_dict_get_item(buildmanifest, "RestoreRamDisks");
			if (node && (plist_get_node_type(node) == PLIST_DICT)) {
				plist_t rd = plist_dict_get_item(node, (client->flags & FLAG_ERASE) ? "User" : "Update");
				if (rd && (plist_get_node_type(rd) == PLIST_STRING)) {
					info = plist_new_dict();
					plist_dict_insert_item(info, "Path", plist_copy(rd));
					comp = plist_new_dict();
					plist_dict_insert_item(comp, "Info", info);
					plist_dict_insert_item(manifest, "RestoreRamDisk", comp);
				}
			}

			// add OS filesystem
			node = plist_dict_get_item(buildmanifest, "SystemRestoreImages");
			if (!node) {
				error("ERROR: missing SystemRestoreImages in Restore.plist\n");
			}
			plist_t os = plist_dict_get_item(node, "User");
			if (!os) {
				error("ERROR: missing filesystem in Restore.plist\n");
			} else {
				info = plist_new_dict();
				plist_dict_insert_item(info, "Path", plist_copy(os));
				comp = plist_new_dict();
				plist_dict_insert_item(comp, "Info", info);
				plist_dict_insert_item(manifest, "OS", comp);
			}

			// finally add manifest
			plist_dict_insert_item(build_identity, "Manifest", manifest);
		}
	} else if (client->flags & FLAG_ERASE) {
		build_identity = build_manifest_get_build_identity(buildmanifest, 0);
		if (build_identity == NULL) {
			error("ERROR: Unable to find any build identities\n");
			plist_free(buildmanifest);
			return -1;
		}
	} else {
		// loop through all build identities in the build manifest
		// and list the valid ones
		int i = 0;
		int valid_builds = 0;
		int build_count = build_manifest_get_identity_count(buildmanifest);
		for (i = 0; i < build_count; i++) {
			build_identity = build_manifest_get_build_identity(buildmanifest, i);
			valid_builds++;
		}
	}

	/* print information about current build identity */
	build_identity_print_information(build_identity);

	/* retrieve shsh blobs if required */
	if (tss_enabled) {
		debug("Getting device's ECID for TSS request\n");
		/* fetch the device's ECID for the TSS request */
		if (get_ecid(client, &client->ecid) < 0) {
			error("ERROR: Unable to find device ECID\n");
			return -1;
		}
		info("Found ECID %llu\n", (long long unsigned int)client->ecid);

		if (get_shsh_blobs(client, client->ecid, NULL, 0, build_identity, &client->tss) < 0) {
			error("ERROR: Unable to get SHSH blobs for this device\n");
			return -1;
		}
	}

	/* verify if we have tss records if required */
	if ((tss_enabled) && (client->tss == NULL)) {
		error("ERROR: Unable to proceed without a TSS record.\n");
		plist_free(buildmanifest);
		return -1;
	}

	if ((tss_enabled) && client->tss) {
		/* fix empty dicts */
		fixup_tss(client->tss);
	}

	// Extract filesystem from IPSW and return its name
	char* filesystem = NULL;
	if (ipsw_extract_filesystem(client->ipsw, build_identity, &filesystem) < 0) {
		error("ERROR: Unable to extract filesystem from IPSW\n");
		if (client->tss)
			plist_free(client->tss);
		plist_free(buildmanifest);
		return -1;
	}

	// if the device is in normal mode, place device into recovery mode
	if (client->mode->index == MODE_NORMAL) {
		info("Entering recovery mode...\n");
		if (normal_enter_recovery(client) < 0) {
			error("ERROR: Unable to place device into recovery mode\n");
			if (client->tss)
				plist_free(client->tss);
			plist_free(buildmanifest);
			return -1;
		}
	}

	// if the device is in DFU mode, place device into recovery mode
	if (client->mode->index == MODE_DFU) {
		recovery_client_free(client);
		if (dfu_enter_recovery(client, build_identity) < 0) {
			error("ERROR: Unable to place device into recovery mode\n");
			plist_free(buildmanifest);
			if (client->tss)
				plist_free(client->tss);
			return -1;
		}
	}

	if (client->mode->index == MODE_DFU) {
		client->mode = &idevicerestore_modes[MODE_RECOVERY];
	} else {
		/* now we load the iBEC */
		if (recovery_send_ibec(client, build_identity) < 0) {
			error("ERROR: Unable to send iBEC\n");
			return -1;
		}
		recovery_client_free(client);
	
		/* this must be long enough to allow the device to run the iBEC */
		/* FIXME: Probably better to detect if the device is back then */
		sleep(7);
	}

	if (client->build[0] > '8') {
		// we need another tss request with nonce.
		unsigned char* nonce = NULL;
		int nonce_size = 0;
		int nonce_changed = 0;
		if (get_nonce(client, &nonce, &nonce_size) < 0) {
			error("ERROR: Unable to get nonce from device!\n");
			recovery_send_reset(client);
			return -1;
		}

		if (!client->nonce || (nonce_size != client->nonce_size) || (memcmp(nonce, client->nonce, nonce_size) != 0)) {
			nonce_changed = 1;
			if (client->nonce) {
				free(client->nonce);
			}
			client->nonce = nonce;
			client->nonce_size = nonce_size;
		} else {
			free(nonce);
		}

		info("Nonce: ");
		int i;
		for (i = 0; i < client->nonce_size; i++) {
			info("%02x ", client->nonce[i]);
		}
		info("\n");

		if (nonce_changed && !(client->flags & FLAG_CUSTOM)) {
			// Welcome iOS5. We have to re-request the TSS with our nonce.
			plist_free(client->tss);
			if (get_shsh_blobs(client, client->ecid, client->nonce, client->nonce_size, build_identity, &client->tss) < 0) {
				error("ERROR: Unable to get SHSH blobs for this device\n");
				return -1;
			}
			if (!client->tss) {
				error("ERROR: can't continue without TSS\n");
				return -1;
			}
			fixup_tss(client->tss);
		}
	}

	// now finally do the magic to put the device into restore mode
	if (client->mode->index == MODE_RECOVERY) {
		if (recovery_enter_restore(client, build_identity) < 0) {
			error("ERROR: Unable to place device into restore mode\n");
			plist_free(buildmanifest);
			if (client->tss)
				plist_free(client->tss);
			return -1;
		}
	}

	// device is finally in restore mode, let's do this
	if (client->mode->index == MODE_RESTORE) {
		info("About to restore device... \n");
		if (restore_device(client, build_identity, filesystem) < 0) {
			error("ERROR: Unable to restore device\n");
			return -1;
		}
	}

	info("Cleaning up...\n");
	if (filesystem)
		unlink(filesystem);

	info("DONE\n");
	return 0;
}

int check_mode(struct idevicerestore_client_t* client) {
	int mode = MODE_UNKNOWN;

	if (recovery_check_mode() == 0) {
		mode = MODE_RECOVERY;
	}

	else if (dfu_check_mode() == 0) {
		mode = MODE_DFU;
	}

	else if (normal_check_mode(client->uuid) == 0) {
		mode = MODE_NORMAL;
	}

	else if (restore_check_mode(client->uuid) == 0) {
		mode = MODE_RESTORE;
	}

	client->mode = &idevicerestore_modes[mode];
	return mode;
}

int check_device(struct idevicerestore_client_t* client) {
	int device = DEVICE_UNKNOWN;
	uint32_t bdid = 0;
	uint32_t cpid = 0;

	switch (client->mode->index) {
	case MODE_RESTORE:
		device = restore_check_device(client->uuid);
		if (device < 0) {
			device = DEVICE_UNKNOWN;
		}
		break;

	case MODE_NORMAL:
		device = normal_check_device(client->uuid);
		if (device < 0) {
			device = DEVICE_UNKNOWN;
		}
		break;

	case MODE_DFU:
	case MODE_RECOVERY:
		if (get_cpid(client, &cpid) < 0) {
			error("ERROR: Unable to get device CPID\n");
			break;
		}

		switch (cpid) {
		case CPID_IPHONE2G:
			// iPhone1,1 iPhone1,2 and iPod1,1 all share the same ChipID
			//   so we need to check the BoardID
			if (get_bdid(client, &bdid) < 0) {
				error("ERROR: Unable to get device BDID\n");
				break;
			}

			switch (bdid) {
			case BDID_IPHONE2G:
				device = DEVICE_IPHONE2G;
				break;

			case BDID_IPHONE3G:
				device = DEVICE_IPHONE3G;
				break;

			case BDID_IPOD1G:
				device = DEVICE_IPOD1G;
				break;

			default:
				device = DEVICE_UNKNOWN;
				break;
			}
			break;

		case CPID_IPHONE3GS:
			device = DEVICE_IPHONE3GS;
			break;

		case CPID_IPOD2G:
			device = DEVICE_IPOD2G;
			break;

		case CPID_IPOD3G:
			device = DEVICE_IPOD3G;
			break;

		case CPID_IPAD1G:
			// All the A4 devices are the same...BoardID'll solve that problem!
			if (get_bdid(client, &bdid) < 0) {
				error("ERROR: Unable to get device BDID\n");
				break;
			}

			switch (bdid) {
			case BDID_IPAD1G:
				device = DEVICE_IPAD1G;
				break;

			case BDID_IPHONE4:
				device = DEVICE_IPHONE4;
				break;

			case BDID_IPOD4G:
				device = DEVICE_IPOD4G;
				break;

			case BDID_APPLETV2:
				device = DEVICE_APPLETV2;
				break;

			case BDID_IPHONE42:
				device = DEVICE_IPHONE42;
				break;

			default:
				device = DEVICE_UNKNOWN;
				break;
			}
			break;

		case CPID_IPAD21:
			// All the A5 devices are the same too...
			if (get_bdid(client, &bdid) < 0) {
				error("ERROR: Unable to get device BDID\n");
				break;
			}

			switch (bdid) {
			case BDID_IPAD21:
				device = DEVICE_IPAD21;
				break;

			case BDID_IPAD22:
				device = DEVICE_IPAD22;
				break;

			case BDID_IPAD23:
				device = DEVICE_IPAD23;
				break;

			default:
				device = DEVICE_UNKNOWN;
				break;
			}
			break;

		default:
			device = DEVICE_UNKNOWN;
			break;
		}
		break;

	default:
		device = DEVICE_UNKNOWN;
		break;

	}

	client->device = &idevicerestore_devices[device];
	return device;
}

int get_bdid(struct idevicerestore_client_t* client, uint32_t* bdid) {
	switch (client->mode->index) {
	case MODE_NORMAL:
		if (normal_get_bdid(client->uuid, bdid) < 0) {
			*bdid = 0;
			return -1;
		}
		break;

	case MODE_DFU:
	case MODE_RECOVERY:
		if (recovery_get_bdid(client, bdid) < 0) {
			*bdid = 0;
			return -1;
		}
		break;

	default:
		error("ERROR: Device is in an invalid state\n");
		return -1;
	}

	return 0;
}

int get_cpid(struct idevicerestore_client_t* client, uint32_t* cpid) {
	switch (client->mode->index) {
	case MODE_NORMAL:
		if (normal_get_cpid(client->uuid, cpid) < 0) {
			client->device->chip_id = -1;
			return -1;
		}
		break;

	case MODE_DFU:
	case MODE_RECOVERY:
		if (recovery_get_cpid(client, cpid) < 0) {
			client->device->chip_id = -1;
			return -1;
		}
		break;

	default:
		error("ERROR: Device is in an invalid state\n");
		return -1;
	}

	return 0;
}

int get_ecid(struct idevicerestore_client_t* client, uint64_t* ecid) {
	switch (client->mode->index) {
	case MODE_NORMAL:
		if (normal_get_ecid(client->uuid, ecid) < 0) {
			*ecid = 0;
			return -1;
		}
		break;

	case MODE_DFU:
	case MODE_RECOVERY:
		if (recovery_get_ecid(client, ecid) < 0) {
			*ecid = 0;
			return -1;
		}
		break;

	default:
		error("ERROR: Device is in an invalid state\n");
		return -1;
	}

	return 0;
}

int get_nonce(struct idevicerestore_client_t* client, unsigned char** nonce, int* nonce_size) {
	*nonce = NULL;
	*nonce_size = 0;

	switch (client->mode->index) {
	case MODE_NORMAL:
		error("ERROR: Can't get nonce in Normal mode\n");
		return -1;
	case MODE_DFU:
		if (dfu_get_nonce(client, nonce, nonce_size) < 0) {
			return -1;
		}
		break;
	case MODE_RECOVERY:
		if (recovery_get_nonce(client, nonce, nonce_size) < 0) {
			return -1;
		}
		break;

	default:
		error("ERROR: Device is in an invalid state\n");
		return -1;
	}

	return 0;
}


plist_t build_manifest_get_build_identity(plist_t build_manifest, uint32_t identity) {
	// fetch build identities array from BuildManifest
	plist_t build_identities_array = plist_dict_get_item(build_manifest, "BuildIdentities");
	if (!build_identities_array || plist_get_node_type(build_identities_array) != PLIST_ARRAY) {
		error("ERROR: Unable to find build identities node\n");
		return NULL;
	}

	// check and make sure this identity exists in buildmanifest
	if (identity >= plist_array_get_size(build_identities_array)) {
		return NULL;
	}

	plist_t build_identity = plist_array_get_item(build_identities_array, identity);
	if (!build_identity || plist_get_node_type(build_identity) != PLIST_DICT) {
		error("ERROR: Unable to find build identities node\n");
		return NULL;
	}

	return plist_copy(build_identity);
}

int get_shsh_blobs(struct idevicerestore_client_t* client, uint64_t ecid, unsigned char* nonce, int nonce_size, plist_t build_identity, plist_t* tss) {
	plist_t request = NULL;
	plist_t response = NULL;
	*tss = NULL;

	request = tss_create_request(build_identity, ecid, nonce, nonce_size);
	if (request == NULL) {
		error("ERROR: Unable to create TSS request\n");
		return -1;
	}

	info("Sending TSS request... ");
	response = tss_send_request(request);
	if (response == NULL) {
		info("ERROR: Unable to send TSS request\n");
		plist_free(request);
		return -1;
	}

	info("received SHSH blobs\n");

	plist_free(request);
	*tss = response;
	return 0;
}

void fixup_tss(plist_t tss)
{
	plist_t node;
	plist_t node2;
	node = plist_dict_get_item(tss, "RestoreLogo");
	if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
		node2 = plist_dict_get_item(tss, "AppleLogo");
		if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
			plist_dict_remove_item(tss, "RestoreLogo");
			plist_dict_insert_item(tss, "RestoreLogo", plist_copy(node2));
		}
	}
	node = plist_dict_get_item(tss, "RestoreDeviceTree");
	if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
		node2 = plist_dict_get_item(tss, "DeviceTree");
		if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
			plist_dict_remove_item(tss, "RestoreDeviceTree");
			plist_dict_insert_item(tss, "RestoreDeviceTree", plist_copy(node2));
		}
	}
	node = plist_dict_get_item(tss, "RestoreKernelCache");
	if (node && (plist_get_node_type(node) == PLIST_DICT) && (plist_dict_get_size(node) == 0)) {
		node2 = plist_dict_get_item(tss, "KernelCache");
		if (node2 && (plist_get_node_type(node2) == PLIST_DICT)) {
			plist_dict_remove_item(tss, "RestoreKernelCache");
			plist_dict_insert_item(tss, "RestoreKernelCache", plist_copy(node2));
		}
	}
}

int build_manifest_get_identity_count(plist_t build_manifest) {
	// fetch build identities array from BuildManifest
	plist_t build_identities_array = plist_dict_get_item(build_manifest, "BuildIdentities");
	if (!build_identities_array || plist_get_node_type(build_identities_array) != PLIST_ARRAY) {
		error("ERROR: Unable to find build identities node\n");
		return -1;
	}

	// check and make sure this identity exists in buildmanifest
	return plist_array_get_size(build_identities_array);
}

int ipsw_extract_filesystem(const char* ipsw, plist_t build_identity, char** filesystem) {
	char* filename = NULL;

	if (build_identity_get_component_path(build_identity, "OS", &filename) < 0) {
		error("ERROR: Unable get path for filesystem component\n");
		return -1;
	}

	info("Extracting filesystem from IPSW\n");
	if (ipsw_extract_to_file(ipsw, filename, filename) < 0) {
		error("ERROR: Unable to extract filesystem\n");
		return -1;
	}

	*filesystem = filename;
	return 0;
}

int ipsw_get_component_by_path(const char* ipsw, plist_t tss, const char* component, const char* path, char** data, uint32_t* size) {
	img3_file* img3 = NULL;
	uint32_t component_size = 0;
	char* component_data = NULL;
	char* component_blob = NULL;
	char* component_name = NULL;

	component_name = strrchr(path, '/');
	if (component_name != NULL)
		component_name++;
	else
		component_name = (char*) path;

	info("Extracting %s\n", component_name);
	if (ipsw_extract_to_memory(ipsw, path, &component_data, &component_size) < 0) {
		error("ERROR: Unable to extract %s from %s\n", component_name, ipsw);
		return -1;
	}

	if (tss) {
		img3 = img3_parse_file(component_data, component_size);
		if (img3 == NULL) {
			error("ERROR: Unable to parse IMG3: %s\n", component_name);
			free(component_data);
			return -1;
		}
		free(component_data);

		/* sign the blob if required */
		if (component) {
			if (tss_get_blob_by_name(tss, component, &component_blob) < 0) {
				error("ERROR: Unable to get SHSH blob for TSS %s entry\n", component_name);
				img3_free(img3);
				return -1;
			}
		} else {
			if (tss_get_blob_by_path(tss, path, &component_blob) < 0) {
				error("ERROR: Unable to get SHSH blob for TSS %s entry\n", component_name);
				img3_free(img3);
				return -1;
			}
		}

		info("Signing %s\n", component_name);
		if (img3_replace_signature(img3, component_blob) < 0) {
			error("ERROR: Unable to replace IMG3 signature\n");
			free(component_blob);
			img3_free(img3);
			return -1;
		}

		if (component_blob)
			free(component_blob);

		if (img3_get_data(img3, &component_data, &component_size) < 0) {
			error("ERROR: Unable to reconstruct IMG3\n");
			img3_free(img3);
			return -1;
		}
		img3_free(img3);
	}

	if (idevicerestore_debug) {
		write_file(component_name, component_data, component_size);
	}

	*data = component_data;
	*size = component_size;
	return 0;
}

int build_manifest_check_compatibility(plist_t build_manifest, const char* product) {
	int res = -1;
	plist_t node = plist_dict_get_item(build_manifest, "SupportedProductTypes");
	if (!node || (plist_get_node_type(node) != PLIST_ARRAY)) {
		debug("%s: ERROR: SupportedProductTypes key missing\n", __func__);
		return -1;
	}
	uint32_t pc = plist_array_get_size(node);
	uint32_t i;
	for (i = 0; i < pc; i++) {
		plist_t prod = plist_array_get_item(node, i);
		if (plist_get_node_type(prod) == PLIST_STRING) {
			char *val = NULL;
			plist_get_string_val(prod, &val);
			if (val && (strcmp(val, product) == 0)) {
				res = 0;
				break;
			}
		}
	}
	return res;
}

void build_manifest_get_version_information(plist_t build_manifest, char** product_version, char** product_build) {
	plist_t node = NULL;
	*product_version = NULL;
	*product_build = NULL;

	node = plist_dict_get_item(build_manifest, "ProductVersion");
	if (!node || plist_get_node_type(node) != PLIST_STRING) {
		error("ERROR: Unable to find ProductVersion node\n");
		return;
	}
	plist_get_string_val(node, product_version);

	node = plist_dict_get_item(build_manifest, "ProductBuildVersion");
	if (!node || plist_get_node_type(node) != PLIST_STRING) {
		error("ERROR: Unable to find ProductBuildVersion node\n");
		return;
	}
	plist_get_string_val(node, product_build);
}

void build_identity_print_information(plist_t build_identity) {
	char* value = NULL;
	plist_t info_node = NULL;
	plist_t node = NULL;

	info_node = plist_dict_get_item(build_identity, "Info");
	if (!info_node || plist_get_node_type(info_node) != PLIST_DICT) {
		error("ERROR: Unable to find Info node\n");
		return;
	}

	node = plist_dict_get_item(info_node, "Variant");
	if (!node || plist_get_node_type(node) != PLIST_STRING) {
		error("ERROR: Unable to find Variant node\n");
		return;
	}
	plist_get_string_val(node, &value);

	info("Variant: %s\n", value);
	free(value);

	node = plist_dict_get_item(info_node, "RestoreBehavior");
	if (!node || plist_get_node_type(node) != PLIST_STRING) {
		error("ERROR: Unable to find RestoreBehavior node\n");
		return;
	}
	plist_get_string_val(node, &value);

	if (!strcmp(value, "Erase"))
		info("This restore will erase your device data.\n");

	if (!strcmp(value, "Update"))
		info("This restore will update your device without losing data.\n");

	free(value);

	info_node = NULL;
	node = NULL;
}

int build_identity_get_component_path(plist_t build_identity, const char* component, char** path) {
	char* filename = NULL;

	plist_t manifest_node = plist_dict_get_item(build_identity, "Manifest");
	if (!manifest_node || plist_get_node_type(manifest_node) != PLIST_DICT) {
		error("ERROR: Unable to find manifest node\n");
		if (filename)
			free(filename);
		return -1;
	}

	plist_t component_node = plist_dict_get_item(manifest_node, component);
	if (!component_node || plist_get_node_type(component_node) != PLIST_DICT) {
		error("ERROR: Unable to find component node for %s\n", component);
		if (filename)
			free(filename);
		return -1;
	}

	plist_t component_info_node = plist_dict_get_item(component_node, "Info");
	if (!component_info_node || plist_get_node_type(component_info_node) != PLIST_DICT) {
		error("ERROR: Unable to find component info node for %s\n", component);
		if (filename)
			free(filename);
		return -1;
	}

	plist_t component_info_path_node = plist_dict_get_item(component_info_node, "Path");
	if (!component_info_path_node || plist_get_node_type(component_info_path_node) != PLIST_STRING) {
		error("ERROR: Unable to find component info path node for %s\n", component);
		if (filename)
			free(filename);
		return -1;
	}
	plist_get_string_val(component_info_path_node, &filename);

	*path = filename;
	return 0;
}

const char* get_component_name(const char* filename)
{
	if (!strncmp(filename, "LLB", 3)) {
		return "LLB";
	} else if (!strncmp(filename, "iBoot", 5)) {
		return "iBoot";
	} else if (!strncmp(filename, "DeviceTree", 10)) {
		return "RestoreDeviceTree";
	} else if (!strncmp(filename, "applelogo", 9)) {
		return "AppleLogo";
	} else if (!strncmp(filename, "recoverymode", 12)) {
		return "RecoveryMode";
	} else if (!strncmp(filename, "batterylow0", 11)) {
		return "BatteryLow0";
	} else if (!strncmp(filename, "batterylow1", 11)) {
		return "BatteryLow1";
	} else if (!strncmp(filename, "glyphcharging", 13)) {
		return "BatteryCharging";
	} else if (!strncmp(filename, "glyphplugin", 11)) {
		return "BatteryPlugin";
	} else if (!strncmp(filename, "batterycharging0", 16)) {
		return "BatteryCharging0";
	} else if (!strncmp(filename, "batterycharging1", 16)) {
		return "BatteryCharging1";
	} else if (!strncmp(filename, "batteryfull", 11)) {
		return "BatteryFull";
	} else if (!strncmp(filename, "SCAB", 4)) {
		return "SCAB";
	} else {
		error("WARNING: Unhandled component '%s'", filename);
		return NULL;
	}
}
