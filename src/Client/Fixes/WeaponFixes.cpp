#pragma once

#include "../../Globals.cpp"
#include "../../Controller/Controller.hpp"
#include "../../Controller/ScreenJoystickHook.hpp"
#include "../../ClientFX/ClientFX.hpp"
#include "../../Server/Server.hpp"

static int actionAnimationThreshold = 0;
static int pUpperAnimationContext = 0;
static bool fireAnimationInterceptionDisabled = false;
static bool requestNextWeapon = false;
static bool requestPreviousWeapon = false;
static bool isFireHeldFromTurret = false;
static DWORD* pWeaponAnimationChanged = nullptr;

// ======================
// WeaponFixes
// ======================

static BYTE* __fastcall AimMgrCtor_Hook(BYTE* thisPtr, int)
{
	g_State.pAimMgr = thisPtr;
	return AimMgrCtor(thisPtr);
}

static void __fastcall UpdateWeaponModel_Hook(DWORD* thisPtr, int)
{
	if (g_State.isEnteringWorld)
	{
		// Set 'm_dwLastWeaponContextAnim' to -1 to force an update of the weapon model
		thisPtr[11] = -1;
		g_State.isEnteringWorld = false;
	}

	pWeaponAnimationChanged = nullptr;
	UpdateWeaponModel(thisPtr);

	// The arms switched to the other posture's version of their animation and continued it, continue the weapon's from the same time instead of restarting it
	DWORD client = EngineInterface::GetClient();
	if (!pWeaponAnimationChanged || !client) return;

	using namespace EngineInterface;
	DWORD modelLT = Call<DWORD>(client, Slot::GetModelLT);
	BYTE* upperContext = reinterpret_cast<BYTE*>(thisPtr[2]);
	uint32_t time = 0;

	// 'm_hObject' and the tracker of the upper body context
	if (CallOffset<int>(modelLT, Model_GetCurAnimTime, *reinterpret_cast<DWORD*>(upperContext + 0xC), (uint32_t)upperContext[0x224], &time) != 0 || time == 0)
		return;

	// Right and left hand models
	for (DWORD hObject : { pWeaponAnimationChanged[7], pWeaponAnimationChanged[53] })
	{
		uint32_t anim = 0, length = 0;
		if (!hObject || CallOffset<int>(modelLT, Model_GetCurAnim, hObject, (uint32_t)0xFF, &anim) != 0)
			continue;

		// Restarted without interpolation, an interpolated animation starts over on the next update
		bool looping = CallOffset<int>(modelLT, Model_GetLooping, hObject, (uint32_t)0xFF) == 86; // LT_YES
		bool playing = CallOffset<int>(modelLT, Model_GetPlaying, hObject, (uint32_t)0xFF) == 86;
		CallOffset<int>(modelLT, Model_SetCurAnim, hObject, (uint32_t)0xFF, anim, false);
		CallOffset<int>(modelLT, Model_ResetAnim, hObject, (uint32_t)0xFF);
		CallOffset<int>(modelLT, Model_SetLooping, hObject, (uint32_t)0xFF, looping);
		CallOffset<int>(modelLT, Model_SetPlaying, hObject, (uint32_t)0xFF, playing);

		if (CallOffset<int>(modelLT, Model_GetCurAnimLength, hObject, (uint32_t)0xFF, &length) == 0 && length != 0)
			CallOffset<int>(modelLT, Model_SetCurAnimTime, hObject, (uint32_t)0xFF, looping ? time % length : (std::min)(time, length));
	}
}

static void __fastcall SetAnimProp_Hook(DWORD* thisPtr, int, int eAnimPropGroup, int eAnimProp)
{
	// Skip if not an action
	if (fireAnimationInterceptionDisabled || eAnimPropGroup != 0)
	{
		SetAnimProp(thisPtr, eAnimPropGroup, eAnimProp);
		return;
	}

	actionAnimationThreshold++;

	// For the first 10 action animations after loading a map
	if (actionAnimationThreshold <= 10)
	{
		// Check if the fire animation is playing
		if (eAnimProp == g_State.kAP_ACT_Fire_Id && pUpperAnimationContext != 0)
		{
			// Unblock the player
			AnimationClearLock(pUpperAnimationContext);
			fireAnimationInterceptionDisabled = true;
		}
	}
	else
	{
		fireAnimationInterceptionDisabled = true;
	}

	SetAnimProp(thisPtr, eAnimPropGroup, eAnimProp);
}

static bool __fastcall InitAnimations_Hook(DWORD* thisPtr, int)
{
	actionAnimationThreshold = 0;
	fireAnimationInterceptionDisabled = false;
	bool res = InitAnimations(thisPtr);
	pUpperAnimationContext = thisPtr[2];
	return res;
}

static void __fastcall NextWeapon_Hook(DWORD* thisPtr, int)
{
	requestNextWeapon = true;
	NextWeapon(thisPtr);
	requestNextWeapon = false;
}

static void __fastcall PreviousWeapon_Hook(DWORD* thisPtr, int)
{
	requestPreviousWeapon = true;
	PreviousWeapon(thisPtr);
	requestPreviousWeapon = false;
}

static uint8_t __fastcall GetWeaponSlot_Hook(int thisPtr, int, int weaponHandle)
{
	// If we're not switching weapons, just call the original
	if (!requestNextWeapon && !requestPreviousWeapon)
	{
		return GetWeaponSlot(thisPtr, weaponHandle);
	}

	// Read the total number of slots and the pointer to the slot array
	uint8_t slotCount = *reinterpret_cast<uint8_t*>(thisPtr + 0x40);
	uint32_t* slotArray = *reinterpret_cast<uint32_t**>(thisPtr + 0xB4);

	// Find the index of the currently held weapon in the slot array
	int currentSlot = -1;
	for (int i = 0; i < slotCount; ++i)
	{
		if (slotArray[i] == static_cast<uint32_t>(weaponHandle))
		{
			currentSlot = i;
			break;
		}
	}

	// Not holding a weapon?
	if (currentSlot < 0)
	{
		if (requestNextWeapon)
		{
			// Position just before the first non-empty slot
			for (int i = 0; i < slotCount; i++)
			{
				if (slotArray[i] != 0)
					return static_cast<uint8_t>(i - 1);
			}
		}

		if (requestPreviousWeapon)
		{
			// Position just after the last non-empty slot
			for (int i = slotCount - 1; i >= 0; i--)
			{
				if (slotArray[i] != 0)
					return static_cast<uint8_t>(i + 1);
			}
		}

		// No weapons at all
		return 0xFF;
	}

	// NextWeapon()
	if (requestNextWeapon)
	{
		int nextIndex = currentSlot + 1;

		// Skip over empty slots
		while (nextIndex < slotCount && slotArray[nextIndex] == 0)
		{
			nextIndex++;
		}

		// If we've run past the end, handle wrapping for original game
		if (nextIndex >= slotCount)
		{
			if (g_State.IsOriginalGame())
			{
				// find first real slot
				int firstReal = -1;
				for (int j = 0; j < slotCount; j++)
				{
					if (slotArray[j] != 0)
					{
						firstReal = j;
						break;
					}
				}
				if (firstReal >= 0)
					return static_cast<uint8_t>(firstReal - 1);
			}
			// For XP/XP2 or if no real slots found, let the engine handle wrapping
			return static_cast<uint8_t>(currentSlot);
		}

		// If we've landed on a real slot, return the previous index for selection
		if (slotArray[nextIndex] != 0)
		{
			return static_cast<uint8_t>(nextIndex - 1);
		}
	}

	// PreviousWeapon()
	if (requestPreviousWeapon)
	{
		int prevIndex = currentSlot - 1;

		// Skip over empty slots
		while (prevIndex >= 0 && slotArray[prevIndex] == 0)
		{
			prevIndex--;
		}

		// If we've run before the beginning, handle wrapping for original game
		if (prevIndex < 0)
		{
			if (g_State.IsOriginalGame())
			{
				// find last real slot
				int lastReal = -1;
				for (int j = slotCount - 1; j >= 0; j--)
				{
					if (slotArray[j] != 0)
					{
						lastReal = j;
						break;
					}
				}
				if (lastReal >= 0)
					return static_cast<uint8_t>(lastReal + 1);
			}
			// For XP/XP2 or if no real slots found, let the engine handle wrapping
			return static_cast<uint8_t>(slotCount);
		}

		// If we've landed on a real slot, return the next index for selection
		if (slotArray[prevIndex] != 0)
		{
			return static_cast<uint8_t>(prevIndex + 1);
		}
	}

	// Fallback: return the actual current slot index
	return static_cast<uint8_t>(currentSlot);
}

static bool __fastcall CanFireWeapon_Hook(int thisPtr, int)
{
	// BC_CFLG_FIRING in the control flags of m_pMoveMgr, set by the button itself
	int moveMgr = *(int*)(thisPtr + 0x14);
	bool isFireHeld = moveMgr != 0 && (*(uint32_t*)(moveMgr + 0x28) & 0x400) != 0;

	// m_pTurretFX
	if (*(int*)(thisPtr + 0x5F4) != 0)
	{
		isFireHeldFromTurret = isFireHeld;
	}
	else if (isFireHeldFromTurret)
	{
		if (isFireHeld)
			return false;

		isFireHeldFromTurret = false;
	}

	return CanFireWeapon(thisPtr);
}

static void __fastcall SetWeaponModelAnimation_Hook(DWORD* thisPtr, int, const char* pszAnimationName)
{
	SetWeaponModelAnimation(thisPtr, pszAnimationName);
	pWeaponAnimationChanged = thisPtr;

	// Play reload animations once, some weapon models have a shorter one than the arms and restart it (SMG magazine pulled out again)
	DWORD client = EngineInterface::GetClient();
	if (!client || !pszAnimationName || !StrStrIA(pszAnimationName, "reload")) return;

	DWORD modelLT = EngineInterface::Call<DWORD>(client, EngineInterface::Slot::GetModelLT);

	// Right and left hand models, 'MAIN_TRACKER'
	for (DWORD hObject : { thisPtr[7], thisPtr[53] })
	{
		if (hObject)
			EngineInterface::CallOffset<int>(modelLT, EngineInterface::Model_SetLooping, hObject, (uint32_t)0xFF, false);
	}
}
