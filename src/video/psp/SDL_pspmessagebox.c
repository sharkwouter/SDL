/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_PSP

#include "SDL_pspvideo.h"
#include "SDL_pspmessagebox.h"
#include <pspkernel.h>
#include <psputility.h>
#include <pspdisplay.h>
#include <pspgu.h>

int PSP_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
    char list[0x20000] __attribute__((aligned(64)));
    pspUtilityMsgDialogParams dialog;
    int init_result;
    int done = 0;
    size_t dialog_size = sizeof(dialog);
    char message[512];

    memset(&dialog, 0, dialog_size);
    dialog.base.size = dialog_size;

    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
				&dialog.base.language); // Prompt language
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,
				&dialog.base.buttonSwap); // X/O button swap

    
    dialog.base.graphicsThread = 0x11;
    dialog.base.accessThread = 0x13;
    dialog.base.fontThread = 0x12;
    dialog.base.soundThread = 0x10;

    dialog.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;

    SDL_snprintf(message, sizeof(message), "%s\r\n\r\n%s", messageboxdata->title, messageboxdata->message);
    strcpy(dialog.message, message);

    if (messageboxdata->numbuttons > 2)
    {
        return -1;
    }

    if (messageboxdata->numbuttons == 2)
    {
        dialog.options |= PSP_UTILITY_MSGDIALOG_OPTION_YESNO_BUTTONS|PSP_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO;
    }

    init_result = sceUtilityMsgDialogInitStart(&dialog);

    if (init_result == 0)
    {
        sceGuInit();
        while (!done)
        {
            sceGuStart(GU_DIRECT,list);
			sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
			sceGuFinish();
			sceGuSync(0,0);

            switch(sceUtilityMsgDialogGetStatus()) {
                case PSP_UTILITY_DIALOG_VISIBLE:
                    sceUtilityMsgDialogUpdate(1);
                    break;
                case PSP_UTILITY_DIALOG_QUIT:
                    sceUtilityMsgDialogShutdownStart();
                    break;
                case PSP_UTILITY_DIALOG_NONE:
                    done=1;
                    break;
            }
            sceDisplayWaitVblankStart();
			sceGuSwapBuffers();
        }

        if (dialog.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_YES)
        {
            *buttonid = messageboxdata->buttons[0].buttonid;
        }
        else if (dialog.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_NO)
        {
            *buttonid = messageboxdata->buttons[1].buttonid;
        }
        else
        {
            *buttonid = messageboxdata->buttons[0].buttonid;
        }
    } else {
        return -1;
    }

    return 0;
}

#endif /* SDL_VIDEO_DRIVER_PSP */

/* vi: set ts=4 sw=4 expandtab: */
