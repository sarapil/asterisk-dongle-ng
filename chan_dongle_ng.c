/*
 * Asterisk Channel Driver for modern 4G/5G modems
 * Project: asterisk-dongle-ng
 *
 * Initial Skeleton File
 *
 * This is the main source file for our channel driver. For now, it only
 * contains the necessary boilerplate code to be recognized as a valid
 * Asterisk module.
 */

/*** MODULEINFO
	<support_level>extended</support_level>
	<default_enable/>
	<language>en</language>
	<description>
		Next-Gen Dongle Channel Driver (chan_dongle_ng)
		Allows communication with 3G/4G/5G modems for voice calls and SMS.
	</description>
	<license>GPLv2</license>
	<author>Your Name</author>
 ***/

#include "asterisk.h" /* Required for all Asterisk modules */

/* Standard Asterisk headers */
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/logger.h"

/*
 * This function is the entry point of the module.
 * It's called by Asterisk when the module is loaded via 'module load'.
 */
static int load_module(void)
{
	/* Log a notice to the Asterisk console */
	ast_log(LOG_NOTICE, "Dongle-NG: Hello World! Module is loading.\n");

	/* In the future, we will initialize devices and data structures here. */

	ast_log(LOG_NOTICE, "Dongle-NG: Module loaded successfully.\n");
	return AST_MODULE_LOAD_SUCCESS;
}

/*
 * This function is the exit point of the module.
 * It's called by Asterisk when the module is unloaded.
 */
static int unload_module(void)
{
	ast_log(LOG_NOTICE, "Dongle-NG: Unloading module.\n");

	/* In the future, we will clean up all resources here. */

	return 0;
}

/*
 * This is the standard Asterisk module definition structure.
 * It tells Asterisk about our module and which functions to call
 * for loading and unloading.
 */
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Next-Gen Dongle Channel Driver",
	.load = load_module,
	.unload = unload_module
);
