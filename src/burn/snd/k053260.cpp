// copyright-holders:Ernesto Corvi, Alex W. Jackson
/*********************************************************

    Konami 053260 KDSC

    The 053260 is a four voice PCM/ADPCM sound chip that
    also incorporates four 8-bit ports for communication
    between a main CPU and audio CPU. The chip's output
    is compatible with a YM3012 DAC, and it has a digital
    auxiliary input compatible with the output of a YM2151.
    Some games (e.g. Simpsons) only connect one channel of
    the YM2151, but others (e.g. Thunder Cross II) connect
    both channels for stereo mixing.

    The 053260 has a 21-bit address bus and 8-bit data bus
    to ROM, allowing it to access up to 2 megabytes of
    sample data. Sample data can be either signed 8-bit
    PCM or a custom 4-bit ADPCM format. It is possible for
    two 053260 chips to share access to the same ROMs
    (used by Over Drive)

    The 053260 has separate address and data buses to the
    audio CPU controlling it and to the main CPU. Both data
    buses are 8 bit. The audio CPU address bus has 6 lines
    (64 addressable registers, but fewer than 48 are
    actually used) while the main CPU "bus" has only 1 line
    (2 addressable registers). All registers on the audio
    CPU side seem to be either read-only or write-only,
    although some games write 0 to all the registers in a
    loop at startup (including otherwise read-only or
    entirely unused registers).
    On the main CPU side, reads and writes to the same
    address access different communication ports.

    The sound data ROMs of Simpsons and Vendetta have
    "headers" listing all the samples in the ROM, their
    formats ("PCM" or "KADPCM"), start and end addresses.
    The header data doesn't seem to be used by the hardware
    (none of the other games have headers) but provides
    useful information about the chip.

    2004-02-28 (Oliver Achten)
    Fixed ADPCM decoding. Games sound much better now.

    2014-10-06 (Alex W. Jackson)
    Rewrote from scratch in C++; implemented communication
    ports properly; used the actual up counters instead of
    converting to fractional sample position; fixed ADPCM
    decoding bugs; added documentation.


*********************************************************/

#include "burnint.h"
#include "k053260.h"

/* 2004-02-28: Fixed ppcm decoding. Games sound much better now.*/

#define BASE_SHIFT	16

#define K053260_INLINE		static inline

static UINT32 nUpdateStep;

struct k053260_channel_def {
	UINT32		rate;
	UINT32		size;
	UINT32		start;
	UINT32		bank;
	UINT32		volume;
	INT32		play;
	UINT32		pan;
	UINT32		pos;
	INT32		loop;
	INT32		ppcm; /* packed PCM ( 4 bit signed ) */
	INT32		ppcm_data;
};

struct k053260_chip_def {
	INT32		mode;
	INT32		regs[0x30];
	UINT8		*rom;
	INT32		rom_size;
	UINT32		*delta_table;
	k053260_channel_def channels[4];

	double		gain[2];
	INT32		output_dir[2];
};

static k053260_chip_def Chips[2];
static k053260_chip_def *ic;

static INT32 nNumChips = 0;

static void InitDeltaTable(INT32 rate, INT32 clock ) {
	INT32		i;
	double	base = ( double )rate;
	double	max = (double)(clock); /* Hz */
	UINT32 val;

	for( i = 0; i < 0x1000; i++ ) {
		double v = ( double )( 0x1000 - i );
		double target = (max) / v;
		double fixed = ( double )( 1 << BASE_SHIFT );

		if ( target && base ) {
			target = fixed / ( base / target );
			val = (UINT32)target;
			if ( val == 0 )
				val = 1;
		} else
			val = 1;

		ic->delta_table[i] = val;
	}
}

void K053260Reset(INT32 chip)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Reset called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K053260Reset called with invalid chip %x\n"), chip);
#endif

	ic = &Chips[chip];

	for(INT32 i = 0; i < 4; i++ ) {
		ic->channels[i].rate = 0;
		ic->channels[i].size = 0;
		ic->channels[i].start = 0;
		ic->channels[i].bank = 0;
		ic->channels[i].volume = 0;
		ic->channels[i].play = 0;
		ic->channels[i].pan = 0;
		ic->channels[i].pos = 0;
		ic->channels[i].loop = 0;
		ic->channels[i].ppcm = 0;
		ic->channels[i].ppcm_data = 0;
	}
}

K053260_INLINE INT32 limit( INT32 val, INT32 max, INT32 min ) {
	if ( val > max )
		val = max;
	else if ( val < min )
		val = min;

	return val;
}

#define MAXOUT 0x3fff
#define MINOUT -0x4000

void K053260Update(INT32 chip, INT16 *pBuf, INT32 length)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Update called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K053260Update called with invalid chip %x\n"), chip);
#endif

	static const INT8 dpcmcnv[] = { 0,1,2,4,8,16,32,64, -128, -64, -32, -16, -8, -4, -2, -1};
	// Pan multipliers.  Set according to integer angles in degrees, amusingly.
	// Exact precision hard to know, the floating point-ish output format makes
	// comparisons iffy.  So we used a 1.16 format.   [O. Galibert, P. Bennett]
	const INT32 pan_mul[8][2] = {
		{     0,     0 }, // No sound for pan 0
		{ 65536,     0 }, //  0 degrees
		{ 59870, 26656 }, // 24 degrees
		{ 53684, 37950 }, // 35 degrees
		{ 46341, 46341 }, // 45 degrees
		{ 37950, 53684 }, // 55 degrees
		{ 26656, 59870 }, // 66 degrees
		{     0, 65536 }  // 90 degrees
	};

	INT32 lvol[4], rvol[4], play[4], loop[4], ppcm[4];
	UINT8 *rom[4];
	UINT32 delta[4], end[4], pos[4];
	INT32 dataL, dataR;
	INT8 ppcm_data[4];
	INT8 d;
	ic = &Chips[chip];

	/* precache some values */
	for ( int i = 0; i < 4; i++ ) {
		rom[i]= &ic->rom[ic->channels[i].start + ( ic->channels[i].bank << 16 ) + 1];
		delta[i] = (ic->delta_table[ic->channels[i].rate] * nUpdateStep) >> 15;
		lvol[i] = ic->channels[i].volume * pan_mul[ic->channels[i].pan][0];
		rvol[i] = ic->channels[i].volume * pan_mul[ic->channels[i].pan][1];
		end[i] = ic->channels[i].size - 1;
		pos[i] = ic->channels[i].pos;
		play[i] = ic->channels[i].play;
		loop[i] = ic->channels[i].loop;
		ppcm[i] = ic->channels[i].ppcm;
		ppcm_data[i] = ic->channels[i].ppcm_data;
		if ( ppcm[i] )
			delta[i] /= 2;
	}

		for ( int j = 0; j < length; j++ ) {

			dataL = dataR = 0;

			for ( int i = 0; i < 4; i++ ) {
				/* see if the voice is on */
				if ( play[i] ) {
					/* see if we're done */
					if ( ( pos[i] >> BASE_SHIFT ) >= end[i] ) {

						ppcm_data[i] = 0;
						if ( loop[i] )
							pos[i] = 0;
						else {
							play[i] = 0;
							continue;
						}
					}

					if ( ppcm[i] ) { /* Packed PCM */
						/* we only update the signal if we're starting or a real sound sample has gone by */
						/* this is all due to the dynamic sample rate convertion */
						if ( pos[i] == 0 || ( ( pos[i] ^ ( pos[i] - delta[i] ) ) & 0x8000 ) == 0x8000 )

						 {
							INT32 newdata;
							if ( pos[i] & 0x8000 ){

								newdata = ((rom[i][pos[i] >> BASE_SHIFT]) >> 4) & 0x0f; /*high nybble*/
							}
							else{
								newdata = ( ( rom[i][pos[i] >> BASE_SHIFT] ) ) & 0x0f; /*low nybble*/
							}

							ppcm_data[i] += dpcmcnv[newdata];
						}

						d = ppcm_data[i];

						pos[i] += delta[i];
					} else { /* PCM */
						d = rom[i][pos[i] >> BASE_SHIFT];

						pos[i] += delta[i];
					}

					if ( ic->mode & 2 ) {
						dataL += ( d * lvol[i] ) >> 16;
						dataR += ( d * rvol[i] ) >> 16;
					}
				}
			}
			
			dataL = limit(dataL, MAXOUT, MINOUT);
			dataR = limit(dataR, MAXOUT, MINOUT);
			
			INT32 nLeftSample = 0, nRightSample = 0;
		
			if ((ic->output_dir[BURN_SND_K053260_ROUTE_1] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
				nLeftSample += (INT32)(dataL * ic->gain[BURN_SND_K053260_ROUTE_1]);
			}
			if ((ic->output_dir[BURN_SND_K053260_ROUTE_1] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
				nRightSample += (INT32)(dataL * ic->gain[BURN_SND_K053260_ROUTE_1]);
			}
		
			if ((ic->output_dir[BURN_SND_K053260_ROUTE_2] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {
				nLeftSample += (INT32)(dataR * ic->gain[BURN_SND_K053260_ROUTE_2]);
			}
			if ((ic->output_dir[BURN_SND_K053260_ROUTE_2] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {
				nRightSample += (INT32)(dataR * ic->gain[BURN_SND_K053260_ROUTE_2]);
			}
		
			nLeftSample = BURN_SND_CLIP(nLeftSample);
			nRightSample = BURN_SND_CLIP(nRightSample);
		
//			pBuf[0] += nLeftSample;
//			pBuf[1] += nRightSample;
			pBuf[0] = BURN_SND_CLIP(pBuf[0] + nLeftSample);
			pBuf[1] = BURN_SND_CLIP(pBuf[1] + nRightSample);
			pBuf += 2;
		}

	/* update the regs now */
	for ( int i = 0; i < 4; i++ ) {
		ic->channels[i].pos = pos[i];
		ic->channels[i].play = play[i];
		ic->channels[i].ppcm_data = ppcm_data[i];
	}
}

void K053260Init(INT32 chip, INT32 clock, UINT8 *rom, INT32 nLen)
{
	DebugSnd_K053260Initted = 1;
	
	ic = &Chips[chip];
	memset (ic, 0, sizeof(k053260_chip_def));

	INT32 rate = clock / 32;
	INT32 i;
	
	if (nBurnSoundRate) nUpdateStep = (INT32)(((float)rate / nBurnSoundRate) * 32768);

	ic->mode = 0;
	ic->rom = rom;
	ic->rom_size = nLen - 1;

	K053260Reset(chip);

	for ( i = 0; i < 0x30; i++ )
		ic->regs[i] = 0;

	ic->delta_table = (UINT32* )BurnMalloc( 0x1000 * sizeof(UINT32) );

	InitDeltaTable( rate, clock );
	
	ic->gain[BURN_SND_K053260_ROUTE_1] = 1.00;
	ic->gain[BURN_SND_K053260_ROUTE_2] = 1.00;
	ic->output_dir[BURN_SND_K053260_ROUTE_1] = BURN_SND_ROUTE_BOTH;
	ic->output_dir[BURN_SND_K053260_ROUTE_2] = BURN_SND_ROUTE_BOTH;
	
	nNumChips = chip;

	/* setup SH1 timer if necessary */
//	if ( ic->intf->irq )
//		timer_pulse( attotime_mul(ATTOTIME_IN_HZ(clock), 32), NULL, 0, ic->intf->irq );
}

void K053260SetRoute(INT32 chip, INT32 nIndex, double nVolume, INT32 nRouteDir)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260SetRoute called without init\n"));
	if (chip >nNumChips) bprintf(PRINT_ERROR, _T("K053260SetRoute called with invalid chip %x\n"), chip);
	if (nIndex < 0 || nIndex > 1) bprintf(PRINT_ERROR, _T("K053260SetRoute called with invalid index %i\n"), nIndex);
#endif

	ic = &Chips[chip];
	
	ic->gain[nIndex] = nVolume;
	ic->output_dir[nIndex] = nRouteDir;
}

void K053260Exit()
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Exit called without init\n"));
#endif

	if (!DebugSnd_K053260Initted) return;

	for (INT32 i = 0; i < 2; i++) {
		ic = &Chips[i];

		BurnFree (ic->delta_table);
	}
	
	nUpdateStep = 0;
	
	DebugSnd_K053260Initted = 0;
	
	nNumChips = 0;
}

K053260_INLINE void check_bounds(INT32 channel ) {

	INT32 channel_start = ( ic->channels[channel].bank << 16 ) + ic->channels[channel].start;
	INT32 channel_end = channel_start + ic->channels[channel].size - 1;

	if ( channel_start > ic->rom_size ) {
		ic->channels[channel].play = 0;

		return;
	}

	if ( channel_end > ic->rom_size ) {
		ic->channels[channel].size = ic->rom_size - channel_start;
	}
}

void K053260Write(INT32 chip, INT32 offset, UINT8 data)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Write called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K053260Write called with invalid chip %x\n"), chip);
#endif

	INT32 i, t;
	INT32 r = offset;
	INT32 v = data;

	ic = &Chips[chip];

	if ( r > 0x2f ) {
		return;
	}

	/* before we update the regs, we need to check for a latched reg */
	if ( r == 0x28 ) {
		t = ic->regs[r] ^ v;

		for ( i = 0; i < 4; i++ ) {
			if ( t & ( 1 << i ) ) {
				if ( v & ( 1 << i ) ) {
					ic->channels[i].play = 1;
					ic->channels[i].pos = 0;
					ic->channels[i].ppcm_data = 0;
					check_bounds( i );
				} else
					ic->channels[i].play = 0;
			}
		}

		ic->regs[r] = v;
		return;
	}

	/* update regs */
	ic->regs[r] = v;

	/* communication registers */
	if ( r < 8 )
		return;

	/* channel setup */
	if ( r < 0x28 ) {
		INT32 channel = ( r - 8 ) / 8;

		switch ( ( r - 8 ) & 0x07 ) {
			case 0: /* sample rate low */
				ic->channels[channel].rate &= 0x0f00;
				ic->channels[channel].rate |= v;
			break;

			case 1: /* sample rate high */
				ic->channels[channel].rate &= 0x00ff;
				ic->channels[channel].rate |= ( v & 0x0f ) << 8;
			break;

			case 2: /* size low */
				ic->channels[channel].size &= 0xff00;
				ic->channels[channel].size |= v;
			break;

			case 3: /* size high */
				ic->channels[channel].size &= 0x00ff;
				ic->channels[channel].size |= v << 8;
			break;

			case 4: /* start low */
				ic->channels[channel].start &= 0xff00;
				ic->channels[channel].start |= v;
			break;

			case 5: /* start high */
				ic->channels[channel].start &= 0x00ff;
				ic->channels[channel].start |= v << 8;
			break;

			case 6: /* bank */
				ic->channels[channel].bank = v & 0xff;
			break;

			case 7: /* volume is 7 bits. Convert to 8 bits now. */
				ic->channels[channel].volume = ( ( v & 0x7f ) << 1 ) | ( v & 1 );
			break;
		}

		return;
	}

	switch( r ) {
		case 0x2a: /* loop, ppcm */
			for ( i = 0; i < 4; i++ )
				ic->channels[i].loop = ( v & ( 1 << i ) ) != 0;

			for ( i = 4; i < 8; i++ )
				ic->channels[i-4].ppcm = ( v & ( 1 << i ) ) != 0;
		break;

		case 0x2c: /* pan */
			ic->channels[0].pan = v & 7;
			ic->channels[1].pan = ( v >> 3 ) & 7;
		break;

		case 0x2d: /* more pan */
			ic->channels[2].pan = v & 7;
			ic->channels[3].pan = ( v >> 3 ) & 7;
		break;

		case 0x2f: /* control */
			ic->mode = v & 7;
			/* bit 0 = read ROM */
			/* bit 1 = enable sound output */
			/* bit 2 = unknown */
		break;
	}
}

UINT8 K053260Read(INT32 chip, INT32 offset)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Read called without init\n"));
	if (chip > nNumChips) bprintf(PRINT_ERROR, _T("K053260Read called with invalid chip %x\n"), chip);
#endif

	ic = & Chips[chip];

	switch ( offset ) {
		case 0x29: /* channel status */
			{
				INT32 i, status = 0;

				for ( i = 0; i < 4; i++ )
					status |= ic->channels[i].play << i;

				return status;
			}
		break;

		case 0x2e: /* read rom */
			if ( ic->mode & 1 ) {
				UINT32 offs = ic->channels[0].start + ( ic->channels[0].pos >> BASE_SHIFT ) + ( ic->channels[0].bank << 16 );

				ic->channels[0].pos += ( 1 << 16 );

				if ( offs > (UINT32)ic->rom_size ) {

					return 0;
				}

				return ic->rom[offs];
			}
		break;
	}

	return ic->regs[offset];
}

void K053260Scan(INT32 nAction, INT32 *)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_K053260Initted) bprintf(PRINT_ERROR, _T("K053260Scan called without init\n"));
#endif

	struct BurnArea ba;
	char szName[32];

	if ((nAction & ACB_DRIVER_DATA) == 0) {
		return;
	}

	for (INT32 i = 0; i < 2; i++) {
		ic = &Chips[i];

		memset(&ba, 0, sizeof(ba));
		sprintf(szName, "k053260 regs %d", i);
		ba.Data		= ic->regs;
		ba.nLen		= sizeof(ic->regs);
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		memset(&ba, 0, sizeof(ba));
		sprintf(szName, "k053260 channels # %d", i);
		ba.Data		= ic->channels;
		ba.nLen		= sizeof(ic->channels);
		ba.nAddress = 0;
		ba.szName	= szName;
		BurnAcb(&ba);

		SCAN_VAR(ic->mode);
	}
}

#undef K053260_INLINE
