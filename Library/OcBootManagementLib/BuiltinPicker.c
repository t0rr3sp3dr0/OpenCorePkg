/** @file
  Builtin picker and password handler.
  
  Copyright (C) 2019, vit9696. All rights reserved.<BR>
  Copyright (C) 2021, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "BootManagementInternal.h"

#include <Guid/AppleFile.h>
#include <Guid/AppleVariable.h>
#include <Guid/OcVariable.h>

#include <IndustryStandard/AppleCsrConfig.h>

#include <Protocol/AppleBootPolicy.h>
#include <Protocol/AppleKeyMapAggregator.h>
#include <Protocol/AppleBeepGen.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/OcAudio.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/OcGuardLib.h>
#include <Library/OcTimerLib.h>
#include <Library/OcTypingLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcRtcLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ResetSystemLib.h>

STATIC INT32                mStatusRow;
STATIC INT32                mStatusColumn;

STATIC INT32                mRunningColumn;

STATIC UINT64               mPreviousTick;

STATIC UINT64               mLoopDelayStart;
STATIC UINT64               mLoopDelayEnd;

#define OC_KB_DBG_MAX_COLUMN           80
#define OC_KB_DBG_DELTA_SAMPLE_COLUMN  0 //40

#if defined(BUILTIN_DEMONSTRATE_TYPING)
#define OC_TYPING_ROW                  2
#else
#define OC_TYPING_ROW                  0
#endif

#define OC_KB_DBG_PRINT_ROW            (OC_TYPING_ROW + 2)

#define OC_KB_DBG_DOWN_ROW             (OC_KB_DBG_PRINT_ROW + 4)
#define OC_KB_DBG_X_ROW                (OC_KB_DBG_PRINT_ROW + 5)
#define OC_KB_DBG_MODIFIERS_ROW        (OC_KB_DBG_PRINT_ROW + 6)

STATIC
VOID
InitKbDebugDisplay (
  VOID
  )
{
  mRunningColumn = 0;

  mLoopDelayStart = 0;
  mLoopDelayEnd = 0;
}

STATIC
VOID
EFIAPI
InstrumentLoopDelay (
  UINT64 LoopDelayStart,
  UINT64 LoopDelayEnd
  )
{
  mLoopDelayStart     = LoopDelayStart;
  mLoopDelayEnd       = LoopDelayEnd;
}

STATIC
VOID
EFIAPI
ShowKbDebugDisplay (
  UINTN                     NumKeysDown,
  UINTN                     NumKeysHeld,
  APPLE_MODIFIER_MAP        Modifiers
  )
{
  CONST CHAR16    *ClearSpace = L"      ";

  UINT64          CurrentTick;

  CHAR16          Code[3]; // includes flush-ahead space, to make progress visible

  Code[1]         = L' ';
  Code[2]         = L'\0';

  CurrentTick     = AsmReadTsc();

  if (mRunningColumn == OC_KB_DBG_DELTA_SAMPLE_COLUMN) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, mStatusRow + OC_KB_DBG_PRINT_ROW + 1);

    Print (
      L"Called delta   = %,Lu%s\n",
      CurrentTick - mPreviousTick,
      ClearSpace);

    Print (L"Loop delta     = %,Lu (@ -%,Lu)%s%s\n",
      mLoopDelayEnd == 0 ? 0 : mLoopDelayEnd - mLoopDelayStart,
      mLoopDelayEnd == 0 ? 0 : CurrentTick - mLoopDelayEnd,
      ClearSpace,
      ClearSpace);
  }

  mPreviousTick = CurrentTick;

  //
  // Show Apple Event keys
  //
  gST->ConOut->SetCursorPosition (gST->ConOut, mRunningColumn, mStatusRow + OC_KB_DBG_DOWN_ROW);
  if (NumKeysDown > 0) {
    Code[0] = L'D';
  } else {
    Code[0] = L' ';
  }
  gST->ConOut->OutputString (gST->ConOut, Code);

  //
  // Show AKMA key held info
  //
  gST->ConOut->SetCursorPosition (gST->ConOut, mRunningColumn, mStatusRow + OC_KB_DBG_X_ROW);
  if (NumKeysHeld > 0) {
    Code[0] = L'X';
  } else {
    Code[0] = L'.';
  }
  gST->ConOut->OutputString (gST->ConOut, Code);

  //
  // Modifiers info
  //
  gST->ConOut->SetCursorPosition (gST->ConOut, mRunningColumn, mStatusRow + OC_KB_DBG_MODIFIERS_ROW);
  if (Modifiers == 0) {
    Code[0] = L' ';
    gST->ConOut->OutputString (gST->ConOut, Code);
  } else {
    Print (L"%X", Modifiers);
  }

  if (++mRunningColumn >= OC_KB_DBG_MAX_COLUMN) {
    mRunningColumn = 0;
  }

  gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
}

STATIC OC_KB_DEBUG_CALLBACKS mSimplePickerKbDebug = {
  InstrumentLoopDelay,
  ShowKbDebugDisplay
};

STATIC
VOID
DisplaySystemMs (
  VOID
  )
{
  UINT64      CurrentMillis;

  CurrentMillis = DivU64x64Remainder (GetTimeInNanoSecond (GetPerformanceCounter ()), 1000000ULL, NULL);
  Print (L"%,Lu]", CurrentMillis);
}

STATIC
CHAR16
GetPickerEntryCursor (
  IN  OC_BOOT_CONTEXT             *BootContext,
  IN  UINT32                      TimeOutSeconds,
  IN  INTN                        ChosenEntry,
  IN  UINTN                       Index,
  IN  OC_MODIFIER_MAP             OcModifiers
  )
{
  if (TimeOutSeconds > 0 && BootContext->DefaultEntry->EntryIndex - 1 == Index) {
    return L'*';
  }
  
  if (ChosenEntry >= 0 && (UINTN) ChosenEntry == Index) {
    return ((OcModifiers & OC_MODIFIERS_SET_DEFAULT) != 0) ? L'+' : L'>';
  }

  return L' ';
}

EFI_STATUS
EFIAPI
OcShowSimpleBootMenu (
  IN  OC_BOOT_CONTEXT             *BootContext,
  IN  OC_BOOT_ENTRY               **BootEntries,
  OUT OC_BOOT_ENTRY               **ChosenBootEntry
  )
{
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  UINTN                              Index;
  UINTN                              Length;
  OC_PICKER_KEY_INFO                 PickerKeyInfo;
  INTN                               ChosenEntry;
  INTN                               OldChosenEntry;
  INT32                              FirstIndexRow;
  INT32                              MillisColumn;
  CHAR16                             EntryCursor;
  CHAR16                             OldEntryCursor;
  CHAR16                             Code[2];
  UINT32                             TimeOutSeconds;
  UINT32                             Count;
  UINT64                             KeyEndTime;
  BOOLEAN                            PlayedOnce;
  BOOLEAN                            PlayChosen;
  BOOLEAN                            IsTyping;
  BOOLEAN                            ModifiersChanged;
#if defined(BUILTIN_DEMONSTRATE_TYPING)
  INT32                              TypingColumn;
  INT32                              TypingStartColumn;
#endif

  Code[1]        = L'\0';

  TimeOutSeconds = BootContext->PickerContext->TimeoutSeconds;
  KeyEndTime     = 0;

  ASSERT (BootContext->DefaultEntry != NULL);
  ChosenEntry    = (INTN) (BootContext->DefaultEntry->EntryIndex - 1);
  OldChosenEntry = ChosenEntry;

  EntryCursor    = L'\0';
  OldEntryCursor = L'\0';

  FirstIndexRow  = -1;
  IsTyping       = FALSE;

  //
  // Used to detect changes.
  //
  PickerKeyInfo.OcModifiers = OC_MODIFIERS_NONE;

  PlayedOnce     = FALSE;
  PlayChosen     = FALSE;

  DEBUG_CODE_BEGIN ();
  if ((BootContext->PickerContext->PickerAttributes & OC_ATTR_DEBUG_DISPLAY) != 0) {
    DEBUG ((DEBUG_INFO, "OCB: Init builtin picker debug\n"));
    InitKbDebugDisplay();
    BootContext->PickerContext->KbDebug = &mSimplePickerKbDebug;
  }
  DEBUG_CODE_END ();

  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: Missing AppleKeyMapAggregator\n"));
    return EFI_UNSUPPORTED;
  }

  Count = (UINT32) BootContext->BootEntryCount;

  if (Count != MIN (Count, OC_INPUT_MAX)) {
    DEBUG ((DEBUG_WARN, "OCB: Cannot display all entries in the menu!\n"));
  }

  OcConsoleControlSetMode (EfiConsoleControlScreenText);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  if (BootContext->PickerContext->ConsoleAttributes != 0) {
    gST->ConOut->SetAttribute (gST->ConOut, BootContext->PickerContext->ConsoleAttributes & 0x7FU);
  }

  //
  // Extension for OpenCore direct text render for faster redraw with custom background.
  //
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->TestString (gST->ConOut, OC_CONSOLE_MARK_CONTROLLED);

  while (TRUE) {
    if (FirstIndexRow != -1) {
      //
      // Incrementally update menu
      //
      if (OldChosenEntry >= 0 && OldChosenEntry != ChosenEntry) {
        gST->ConOut->SetCursorPosition (gST->ConOut, 0, FirstIndexRow + OldChosenEntry);
        gST->ConOut->OutputString (gST->ConOut, L" ");
      }
      
      if (ChosenEntry >= 0) {
        EntryCursor = GetPickerEntryCursor(BootContext, TimeOutSeconds, ChosenEntry, ChosenEntry, PickerKeyInfo.OcModifiers);
      } else {
        EntryCursor = L'\0';
      }

      if (OldChosenEntry != ChosenEntry || OldEntryCursor != EntryCursor) {
        if (ChosenEntry >= 0) {
          gST->ConOut->SetCursorPosition (gST->ConOut, 0, FirstIndexRow + ChosenEntry);
          Code[0] = EntryCursor;
          gST->ConOut->OutputString (gST->ConOut, Code);
        }

        OldChosenEntry = ChosenEntry;
        OldEntryCursor = EntryCursor;

        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
      }

      DEBUG_CODE_BEGIN ();
      if ((BootContext->PickerContext->PickerAttributes & OC_ATTR_DEBUG_DISPLAY) != 0) {
        //
        // Varying part of milliseconds display
        //
        gST->ConOut->SetCursorPosition (gST->ConOut, MillisColumn, 0);
        DisplaySystemMs ();
        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
      }
      DEBUG_CODE_END ();
    } else {
      //
      // Render initial menu
      //
      gST->ConOut->ClearScreen (gST->ConOut);
      gST->ConOut->OutputString (gST->ConOut, OC_MENU_BOOT_MENU);

      if (BootContext->PickerContext->TitleSuffix != NULL) {
        Length = AsciiStrLen (BootContext->PickerContext->TitleSuffix);
        gST->ConOut->OutputString (gST->ConOut, L" (");
        for (Index = 0; Index < Length; ++Index) {
          Code[0] = BootContext->PickerContext->TitleSuffix[Index];
          gST->ConOut->OutputString (gST->ConOut, Code);
        }
        gST->ConOut->OutputString (gST->ConOut, L")");
      }

      DEBUG_CODE_BEGIN ();
      if ((BootContext->PickerContext->PickerAttributes & OC_ATTR_DEBUG_DISPLAY) != 0) {
        //
        // Fixed part of milliseconds display
        //
        gST->ConOut->OutputString (gST->ConOut, L" [System uptime: ");
        MillisColumn = gST->ConOut->Mode->CursorColumn;
        DisplaySystemMs ();
      }
      DEBUG_CODE_END ();

      gST->ConOut->OutputString (gST->ConOut, L"\r\n\r\n");

      FirstIndexRow = gST->ConOut->Mode->CursorRow;

      for (Index = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
        EntryCursor = GetPickerEntryCursor(BootContext, TimeOutSeconds, ChosenEntry, Index, PickerKeyInfo.OcModifiers);

        if (ChosenEntry >= 0 && (UINTN) ChosenEntry == Index) {
          OldEntryCursor = EntryCursor;
        }

        Code[0] = EntryCursor;
        gST->ConOut->OutputString (gST->ConOut, Code);
        gST->ConOut->OutputString (gST->ConOut, L" ");

        Code[0] = OC_INPUT_STR[Index];
        gST->ConOut->OutputString (gST->ConOut, Code);
        gST->ConOut->OutputString (gST->ConOut, L". ");
        gST->ConOut->OutputString (gST->ConOut, BootEntries[Index]->Name);
        if (BootEntries[Index]->IsExternal) {
          gST->ConOut->OutputString (gST->ConOut, OC_MENU_EXTERNAL);
        }
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
      }

      gST->ConOut->OutputString (gST->ConOut, L"\r\n");
      gST->ConOut->OutputString (gST->ConOut, OC_MENU_CHOOSE_OS);

      mStatusRow     = gST->ConOut->Mode->CursorRow;
      mStatusColumn  = gST->ConOut->Mode->CursorColumn;

#if defined(BUILTIN_DEMONSTRATE_TYPING)
      gST->ConOut->OutputString (gST->ConOut, L"\r\n\r\n");
      Print (L"Typing: ");
      TypingColumn = gST->ConOut->Mode->CursorColumn;
      TypingStartColumn = TypingColumn;
#endif

      DEBUG_CODE_BEGIN ();
      if ((BootContext->PickerContext->PickerAttributes & OC_ATTR_DEBUG_DISPLAY) != 0) {
        //
        // Parts of main debug display which do not need to reprint every frame
        // TODO: Could do more here
        //
        gST->ConOut->OutputString (gST->ConOut, L"\r\n\r\n");
        Print (
          L"mTscFrequency  = %,Lu\n",
          GetTscFrequency ());
        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
      }
      DEBUG_CODE_END ();
    }

    if (!PlayedOnce && BootContext->PickerContext->PickerAudioAssist) {
      OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileChooseOS, FALSE);
      for (Index = 0; Index < Count; ++Index) {
        OcPlayAudioEntry (BootContext->PickerContext, BootEntries[Index]);
        if (TimeOutSeconds > 0 && BootContext->DefaultEntry->EntryIndex - 1 == Index) {
          OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileDefault, FALSE);
        }
      }
      OcPlayAudioBeep (
        BootContext->PickerContext,
        OC_VOICE_OVER_SIGNALS_NORMAL,
        OC_VOICE_OVER_SIGNAL_NORMAL_MS,
        OC_VOICE_OVER_SILENCE_NORMAL_MS
        );
      PlayedOnce = TRUE;
    }

    while (TRUE) {
      if (PlayChosen) {
        //
        // Pronounce entry name only after N ms of idleness.
        //
        KeyEndTime = BootContext->PickerContext->HotKeyContext->GetKeyWaitEndTime (OC_VOICE_OVER_IDLE_TIMEOUT_MS);
      } else if (TimeOutSeconds != 0) {
        if (KeyEndTime == 0) {
          KeyEndTime = BootContext->PickerContext->HotKeyContext->GetKeyWaitEndTime (TimeOutSeconds * 1000);
        }
      } else {
        KeyEndTime = 0;
      }

      ModifiersChanged = BootContext->PickerContext->HotKeyContext->WaitForKeyInfo (
        BootContext->PickerContext,
        KeyEndTime,
        IsTyping ? OC_PICKER_KEYS_FOR_TYPING : OC_PICKER_KEYS_FOR_PICKER,
        &PickerKeyInfo
        );

#if defined(BUILTIN_DEMONSTRATE_TYPING)
      if (PickerKeyInfo.OcKeyCode == OC_INPUT_SWITCH_CONTEXT) {
        //
        // Only allow TAB to go forwards and SHIFT+TAB to go backwards, just to test that it is working.
        //
        if (!IsTyping && ((PickerKeyInfo.OcModifiers & OC_MODIFIERS_REVERSE_SWITCH_CONTEXT) == 0)) {
          IsTyping = TRUE;
        } else if (IsTyping && ((PickerKeyInfo.OcModifiers & OC_MODIFIERS_REVERSE_SWITCH_CONTEXT) != 0)) {
          IsTyping = FALSE;
        }

        //
        // Show/hide typing cursor.
        //
        gST->ConOut->SetCursorPosition (gST->ConOut, TypingColumn, mStatusRow + OC_TYPING_ROW);
        Code[0] = IsTyping ? L'_' : ' ';
        gST->ConOut->OutputString (gST->ConOut, Code);

        //
        // Show/hide picker cursor.
        //
        if (ChosenEntry >= 0) {
          gST->ConOut->SetCursorPosition (gST->ConOut, 0, FirstIndexRow + ChosenEntry);
          Code[0] = IsTyping ? L' ' : OldEntryCursor;
          gST->ConOut->OutputString (gST->ConOut, Code);

          //
          // Timeout gets cancelled and thefore cursor gets redrawn unless we do this.
          //
          OldEntryCursor = GetPickerEntryCursor(BootContext, 0, ChosenEntry, ChosenEntry, PickerKeyInfo.OcModifiers);
        }

        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
      }

      if (PickerKeyInfo.OcKeyCode == OC_INPUT_TYPING_BACKSPACE && TypingColumn > TypingStartColumn) {
        //
        // Backspace and move cursor.
        //
        TypingColumn--;
        gST->ConOut->SetCursorPosition (gST->ConOut, TypingColumn, mStatusRow + OC_TYPING_ROW);
        Code[0] = L'_';
        gST->ConOut->OutputString (gST->ConOut, Code);
        Code[0] = L' ';
        gST->ConOut->OutputString (gST->ConOut, Code);
        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
      } else if (PickerKeyInfo.UnicodeChar >= 32 && PickerKeyInfo.UnicodeChar < 128) {
        //
        // Type and move cursor.
        //
        gST->ConOut->SetCursorPosition (gST->ConOut, TypingColumn, mStatusRow + OC_TYPING_ROW);
        Code[0] = (CHAR16) PickerKeyInfo.UnicodeChar;
        gST->ConOut->OutputString (gST->ConOut, Code);
        Code[0] = L'_';
        gST->ConOut->OutputString (gST->ConOut, Code);
        gST->ConOut->SetCursorPosition (gST->ConOut, mStatusColumn, mStatusRow);
        TypingColumn++;
      }
#endif

      if (PlayChosen && PickerKeyInfo.OcKeyCode == OC_INPUT_TIMEOUT) {
        OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileSelected, FALSE);
        OcPlayAudioEntry (BootContext->PickerContext, BootEntries[ChosenEntry]);
        PlayChosen = FALSE;
        continue;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_TIMEOUT) {
        *ChosenBootEntry = BootEntries[BootContext->DefaultEntry->EntryIndex - 1];
        gST->ConOut->OutputString (gST->ConOut, OC_MENU_TIMEOUT);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
        OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileTimeout, FALSE);
        return EFI_SUCCESS;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_CONTINUE) {
        if (ChosenEntry >= 0) {
          *ChosenBootEntry = BootEntries[(UINTN) ChosenEntry];
        } else {
          *ChosenBootEntry = BootContext->DefaultEntry;
        }
        (*ChosenBootEntry)->SetDefault = ((PickerKeyInfo.OcModifiers & OC_MODIFIERS_SET_DEFAULT) != 0);
        gST->ConOut->OutputString (gST->ConOut, OC_MENU_OK);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
        return EFI_SUCCESS;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_ABORTED) {
        gST->ConOut->OutputString (gST->ConOut, OC_MENU_RELOADING);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
        OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileReloading, FALSE);
        return EFI_ABORTED;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_MORE && BootContext->PickerContext->HideAuxiliary) {
        gST->ConOut->OutputString (gST->ConOut, OC_MENU_SHOW_AUXILIARY);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
        OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileShowAuxiliary, FALSE);
        BootContext->PickerContext->HideAuxiliary = FALSE;
        return EFI_ABORTED;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_UP) {
        if (ChosenEntry < 0) {
          ChosenEntry = 0;
        } else if (ChosenEntry == 0) {
          ChosenEntry = (INTN) (MIN (Count, OC_INPUT_MAX) - 1);
        } else {
          --ChosenEntry;
        }
        TimeOutSeconds = 0;
        if (BootContext->PickerContext->PickerAudioAssist) {
          PlayChosen = TRUE;
        }
        break;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_DOWN) {
        if (ChosenEntry < 0) {
          ChosenEntry = 0;
        } else if (ChosenEntry == (INTN) (MIN (Count, OC_INPUT_MAX) - 1)) {
          ChosenEntry = 0;
        } else {
          ++ChosenEntry;
        }
        TimeOutSeconds = 0;
        if (BootContext->PickerContext->PickerAudioAssist) {
          PlayChosen = TRUE;
        }
        break;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_TOP || PickerKeyInfo.OcKeyCode == OC_INPUT_LEFT) {
        ChosenEntry = 0;
        TimeOutSeconds = 0;
        if (BootContext->PickerContext->PickerAudioAssist) {
          PlayChosen = TRUE;
        }
        break;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_BOTTOM || PickerKeyInfo.OcKeyCode == OC_INPUT_RIGHT) {
        ChosenEntry = (INTN) (MIN (Count, OC_INPUT_MAX) - 1);
        TimeOutSeconds = 0;
        if (BootContext->PickerContext->PickerAudioAssist) {
          PlayChosen = TRUE;
        }
        break;
      } else if (PickerKeyInfo.OcKeyCode == OC_INPUT_VOICE_OVER) {
        OcToggleVoiceOver (BootContext->PickerContext, 0);
        break;
      } else if (PickerKeyInfo.OcKeyCode != OC_INPUT_NO_ACTION && PickerKeyInfo.OcKeyCode >= 0 && (UINTN)PickerKeyInfo.OcKeyCode < Count) {
        *ChosenBootEntry = BootEntries[PickerKeyInfo.OcKeyCode];
        (*ChosenBootEntry)->SetDefault = ((PickerKeyInfo.OcModifiers & OC_MODIFIERS_SET_DEFAULT) != 0);
        Code[0] = OC_INPUT_STR[PickerKeyInfo.OcKeyCode];
        gST->ConOut->OutputString (gST->ConOut, Code);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
        return EFI_SUCCESS;
      }

      if (PickerKeyInfo.OcKeyCode != OC_INPUT_NO_ACTION && TimeOutSeconds > 0) {
        OcPlayAudioFile (BootContext->PickerContext, OcVoiceOverAudioFileAbortTimeout, FALSE);
        TimeOutSeconds = 0;
        break;
      }

      if (ModifiersChanged || PickerKeyInfo.UnicodeChar != CHAR_NULL) {
        break;
      }
    }
  }

  ASSERT (FALSE);
}

EFI_STATUS
EFIAPI
OcShowSimplePasswordRequest (
  IN OC_PICKER_CONTEXT   *Context,
  IN OC_PRIVILEGE_LEVEL  Level
  )
{
  BOOLEAN              Result;

  UINT8                Password[OC_PASSWORD_MAX_LEN];
  UINT32               PwIndex;

  UINT8                Index;
  OC_PICKER_KEY_INFO   PickerKeyInfo;
  UINT8                SpaceIndex;

  OcConsoleControlSetMode (EfiConsoleControlScreenText);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->TestString (gST->ConOut, OC_CONSOLE_MARK_CONTROLLED);

  for (Index = 0; Index < OC_PASSWORD_MAX_RETRIES; ++Index) {
    PwIndex = 0;

    Context->HotKeyContext->FlushTypingBuffer (Context);

    gST->ConOut->OutputString (gST->ConOut, OC_MENU_PASSWORD_REQUEST);
    OcPlayAudioFile (Context, OcVoiceOverAudioFileEnterPassword, TRUE);

    while (TRUE) {
      Context->HotKeyContext->WaitForKeyInfo (
        Context,
        0,
        OC_PICKER_KEYS_FOR_TYPING,
        &PickerKeyInfo
        );

      /*
      if (Status == EFI_NOT_READY) {
        continue;
      } else if (EFI_ERROR (Status)) {
        gST->ConOut->ClearScreen (gST->ConOut);
        SecureZeroMem (Password, PwIndex);
        SecureZeroMem (&Key.UnicodeChar, sizeof (Key.UnicodeChar));

        DEBUG ((DEBUG_ERROR, "Input device error\r\n"));
        OcPlayAudioBeep (
          Context,
          OC_VOICE_OVER_SIGNALS_HWERROR,
          OC_VOICE_OVER_SIGNAL_ERROR_MS,
          OC_VOICE_OVER_SILENCE_ERROR_MS
          );
        return EFI_ABORTED;
      }
      */

      if (PickerKeyInfo.OcKeyCode == OC_INPUT_VOICE_OVER) {
        OcToggleVoiceOver (Context, OcVoiceOverAudioFileEnterPassword);
      }

      if (PickerKeyInfo.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        //
        // RETURN finalizes the input.
        //
        break;
      } else if (PickerKeyInfo.UnicodeChar == CHAR_BACKSPACE) {
        //
        // Delete the last entered character, if such exists.
        //
        if (PwIndex != 0) {
          --PwIndex;
          Password[PwIndex] = 0;
          //
          // Overwrite current character with a space.
          //
          gST->ConOut->SetCursorPosition (
                         gST->ConOut,
                         gST->ConOut->Mode->CursorColumn - 1,
                         gST->ConOut->Mode->CursorRow
                         );
          gST->ConOut->OutputString (gST->ConOut, L" ");
          gST->ConOut->SetCursorPosition (
                         gST->ConOut,
                         gST->ConOut->Mode->CursorColumn - 1,
                         gST->ConOut->Mode->CursorRow
                         );
        }

        OcPlayAudioFile (Context, AppleVoiceOverAudioFileBeep, TRUE);
        continue;
      } else if (PickerKeyInfo.UnicodeChar == CHAR_NULL
       || (UINT8)PickerKeyInfo.UnicodeChar != PickerKeyInfo.UnicodeChar) {
        //
        // Only ASCII characters are supported.
        //
        OcPlayAudioBeep (
          Context,
          OC_VOICE_OVER_SIGNALS_ERROR,
          OC_VOICE_OVER_SIGNAL_ERROR_MS,
          OC_VOICE_OVER_SILENCE_ERROR_MS
          );
        continue;
      }

      if (PwIndex == ARRAY_SIZE (Password)) {
        OcPlayAudioBeep (
          Context,
          OC_VOICE_OVER_SIGNALS_ERROR,
          OC_VOICE_OVER_SIGNAL_ERROR_MS,
          OC_VOICE_OVER_SILENCE_ERROR_MS
          );
        continue;
      }

      gST->ConOut->OutputString (gST->ConOut, L"*");
      Password[PwIndex] = (UINT8)PickerKeyInfo.UnicodeChar;
      OcPlayAudioFile (Context, AppleVoiceOverAudioFileBeep, TRUE);
      ++PwIndex;
    }
    //
    // Output password processing status.
    //
    gST->ConOut->SetCursorPosition (
      gST->ConOut,
      0,
      gST->ConOut->Mode->CursorRow
      );
    gST->ConOut->OutputString (gST->ConOut, OC_MENU_PASSWORD_PROCESSING);
    //
    // Clear remaining password prompt status.
    //
    for (
      SpaceIndex = L_STR_LEN (OC_MENU_PASSWORD_PROCESSING);
      SpaceIndex < L_STR_LEN (OC_MENU_PASSWORD_REQUEST) + PwIndex;
      ++SpaceIndex
      ) {
      gST->ConOut->OutputString (gST->ConOut, L" ");
    }

    Result = OcVerifyPasswordSha512 (
               Password,
               PwIndex,
               Context->PrivilegeContext->Salt,
               Context->PrivilegeContext->SaltSize,
               Context->PrivilegeContext->Hash
               );

    SecureZeroMem (Password, PwIndex);
    //
    // Clear password processing status.
    //
    gST->ConOut->SetCursorPosition (
      gST->ConOut,
      0,
      gST->ConOut->Mode->CursorRow
      );
    for (SpaceIndex = 0; SpaceIndex < L_STR_LEN (OC_MENU_PASSWORD_PROCESSING); ++SpaceIndex) {
      gST->ConOut->OutputString (gST->ConOut, L" ");
    }
    gST->ConOut->SetCursorPosition (
      gST->ConOut,
      0,
      gST->ConOut->Mode->CursorRow
      );

    if (Result) {
      OcPlayAudioFile (Context, OcVoiceOverAudioFilePasswordAccepted, TRUE);
      return EFI_SUCCESS;
      break;
    } else {
      OcPlayAudioFile (Context, OcVoiceOverAudioFilePasswordIncorrect, TRUE);
    }
  }

  gST->ConOut->OutputString (gST->ConOut, OC_MENU_PASSWORD_RETRY_LIMIT L"\r\n");
  OcPlayAudioFile (Context, OcVoiceOverAudioFilePasswordRetryLimit, TRUE);
  DEBUG ((DEBUG_WARN, "OCB: User failed to verify password %d times running\n", OC_PASSWORD_MAX_RETRIES));

  gBS->Stall (SECONDS_TO_MICROSECONDS (5));
  ResetWarm ();

  return EFI_ACCESS_DENIED;
}
