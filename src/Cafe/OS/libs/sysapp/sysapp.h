#include "Cafe/OS/RPL/COSModule.h"

namespace sysapp
{
	COSModule* GetModule();
	// switches to the installed applet with the given PFID (e.g. 9 = Miiverse), like SYSSwitchTo. Returns 0 on success
	uint32 SwitchToPFID(sint32 pfid, const char* funcName);
}

uint64 _SYSGetSystemApplicationTitleId(sint32 index);