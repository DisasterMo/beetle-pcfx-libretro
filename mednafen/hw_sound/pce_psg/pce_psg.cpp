/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <assert.h>
#include <math.h>
#include <string.h>

#include "mednafen/mednafen.h"
#include "mednafen/state_helpers.h"
#include "pce_psg.h"

// Frequency cache cutoff optimization threshold (<= FREQC7M_COT)
#define FREQC7M_COT	0x7 //0xA

void PCE_PSG::SetVolume(double new_volume)
{
   for(int vl = 0; vl < 32; vl++)
   {
      double flub = 1.0 * new_volume * 8 / 6;

      if(vl)
         flub /= powf(2, (double)1 / 4 * vl);                  // ~1.5dB reduction per increment of vl 

      if(vl == 0x1F)
         flub = 0;

      for(int samp = 0; samp < 32; samp++)
      {
         int eff_samp;

         if(revision == REVISION_HUC6280)
            eff_samp = samp * 2;
         else
            eff_samp = samp * 2 - 0x1F;

         dbtable[vl][samp] = (int32)(flub * eff_samp * 128); // * 256);
         dbtable_volonly[vl] = (int32)(flub * 65536);

         // dbtable[vl][samp] = (int32)(flub * eff_samp * 128);
         // dbtable_volonly[vl] = (int32)(flub * 65536);
      }
   }
}

// Note: Changing the 0x1F(not that there should be) would require changing the channel pseudo-off volume check logic later on.
static const int scale_tab[] = 
{
   0x00, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F,
   0x10, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D, 0x1F
};

#define CLOCK_LFSR(lfsr) { unsigned int newbit = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 11) ^ (lfsr >> 12) ^ (lfsr >> 17)) & 1; lfsr = (lfsr >> 1) | (newbit << 17); }

static const int16 Phase_Filter[2][7] =
{
 /*   0 */ {    35,   250,   579,   641,   425,   112,     6 }, //  2048
 /*   1 */ {     6,   112,   425,   641,   579,   250,    35 }, //  2048
};

INLINE void PCE_PSG::UpdateOutputSub(const int32 timestamp, psg_channel *ch, const int32 samp0, const int32 samp1)
{
 int32 delta[2];

 delta[0] = samp0 - ch->blip_prev_samp[0];
 delta[1] = samp1 - ch->blip_prev_samp[1];

 const int16* c = Phase_Filter[(timestamp >> 1) & 1];
 const int32 l = (timestamp >> 2) & 0xFFFF;

 HRBufs[0][l + 0] += delta[0] * c[0];
 HRBufs[0][l + 1] += delta[0] * c[1];
 HRBufs[0][l + 2] += delta[0] * c[2];
 HRBufs[0][l + 3] += delta[0] * c[3];
 HRBufs[0][l + 4] += delta[0] * c[4];
 HRBufs[0][l + 5] += delta[0] * c[5];
 HRBufs[0][l + 6] += delta[0] * c[6];

 HRBufs[1][l + 0] += delta[1] * c[0];
 HRBufs[1][l + 1] += delta[1] * c[1];
 HRBufs[1][l + 2] += delta[1] * c[2];
 HRBufs[1][l + 3] += delta[1] * c[3];
 HRBufs[1][l + 4] += delta[1] * c[4];
 HRBufs[1][l + 5] += delta[1] * c[5];
 HRBufs[1][l + 6] += delta[1] * c[6];

 ch->blip_prev_samp[0] = samp0;
 ch->blip_prev_samp[1] = samp1;
}

void PCE_PSG::UpdateOutput_Norm(const int32 timestamp, psg_channel *ch)
{
 int sv = ch->dda;

 UpdateOutputSub(timestamp, ch, dbtable[ch->vl[0]][sv],
 				dbtable[ch->vl[1]][sv]);
}

void PCE_PSG::UpdateOutput_Noise(const int32 timestamp, psg_channel *ch)
{
 int sv = ((ch->lfsr & 1) << 5) - (ch->lfsr & 1); //(ch->lfsr & 0x1) ? 0x1F : 0;

 UpdateOutputSub(timestamp, ch, dbtable[ch->vl[0]][sv],
				dbtable[ch->vl[1]][sv]);
}

void PCE_PSG::UpdateOutput_Off(const int32 timestamp, psg_channel *ch)
{
 UpdateOutputSub(timestamp, ch, 0, 0);
}

void PCE_PSG::UpdateOutput_Accum_HuC6280A(const int32 timestamp, psg_channel *ch)
{
 int32 samp[2];

 // 31(5-bit max) * 32 samples = 992
 // 992 / 2 = 496
 // 
 // 8 + 5 = 13
 // 13 - 12 = 1

 samp[0] = ((int32)dbtable_volonly[ch->vl[0]] * ((int32)ch->samp_accum - 496)) >> (8 + 5);
 samp[1] = ((int32)dbtable_volonly[ch->vl[1]] * ((int32)ch->samp_accum - 496)) >> (8 + 5);

 UpdateOutputSub(timestamp, ch, samp[0], samp[1]);
}

void PCE_PSG::UpdateOutput_Accum_HuC6280(const int32 timestamp, psg_channel *ch)
{
 int32 samp[2];

 samp[0] = ((int32)dbtable_volonly[ch->vl[0]] * (int32)ch->samp_accum) >> (8 + 5);
 samp[1] = ((int32)dbtable_volonly[ch->vl[1]] * (int32)ch->samp_accum) >> (8 + 5);

 UpdateOutputSub(timestamp, ch, samp[0], samp[1]);
}


// This function should always be called after RecalcFreqCache() (it's not called from RecalcFreqCache to avoid redundant code)
void PCE_PSG::RecalcUOFunc(int chnum)
{
 psg_channel *ch = &channel[chnum];

 if((revision != REVISION_HUC6280 && !(ch->control & 0xC0)) || (revision == REVISION_HUC6280 && !(ch->control & 0x80)))
  ch->UpdateOutput = &PCE_PSG::UpdateOutput_Off;
 else if(ch->noisectrl & ch->control & 0x80)
  ch->UpdateOutput = &PCE_PSG::UpdateOutput_Noise;
 // If the control for the channel is in waveform play mode, and the (real) playback frequency is too high, and the channel is either not the LFO modulator channel or
 // if the LFO trigger bit(which halts the LFO modulator channel's waveform incrementing when set) is clear
 else if((ch->control & 0xC0) == 0x80 && ch->freq_cache <= FREQC7M_COT && (chnum != 1 || !(lfoctrl & 0x80)) )
  ch->UpdateOutput = UpdateOutput_Accum;
 else
  ch->UpdateOutput = &PCE_PSG::UpdateOutput_Norm;
}


void PCE_PSG::RecalcFreqCache(int chnum)
{
 psg_channel *ch = &channel[chnum];

 if(chnum == 0 && (lfoctrl & 0x03))
 {
  const uint32 shift = (((lfoctrl & 0x3) - 1) << 1);
  uint8 la = channel[1].dda;
  int32 tmp_freq = ((int32)ch->frequency + ((la - 0x10) << shift)) & 0xFFF;

  ch->freq_cache = (tmp_freq ? tmp_freq : 4096) << 1;
 }
 else
 {
  ch->freq_cache = (ch->frequency ? ch->frequency : 4096) << 1;

  if(chnum == 1 && (lfoctrl & 0x03))
   ch->freq_cache *= lfofreq ? lfofreq : 256;
 }
}

void PCE_PSG::RecalcNoiseFreqCache(int chnum)
{
 psg_channel *ch = &channel[chnum];
 int32 freq = 0x1F - (ch->noisectrl & 0x1F);

 if(!freq)
  freq = 0x20;
 else
  freq <<= 6;

 freq <<= 1;

 ch->noise_freq_cache = freq;
}

void PCE_PSG::PeekWave(const unsigned int ch, uint32 Address, uint32 Length, uint8 *Buffer)
{
 assert(ch <= 5);

 while(Length--)
 {
  Address &= 0x1F;
  *Buffer = channel[ch].waveform[Address];
  Address++;
  Buffer++;
 }
}

void PCE_PSG::PokeWave(const unsigned int ch, uint32 Address, uint32 Length, const uint8 *Buffer)
{
 assert(ch <= 5);

 while(Length--)
 {
  Address &= 0x1F;
  channel[ch].samp_accum -= channel[ch].waveform[Address];
  channel[ch].waveform[Address] = *Buffer & 0x1F;
  channel[ch].samp_accum += channel[ch].waveform[Address];
  Address++;
  Buffer++;
 }
}

uint32 PCE_PSG::GetRegister(const unsigned int id)
{
 uint32 value = 0xDEADBEEF;
 const int ch = (id >> 8) & 0xF;

 switch(id & 0xF0FF)
 {
  default: break;

  case PSG_GSREG_SELECT:
	value = select;
	break;

  case PSG_GSREG_GBALANCE:
	value = globalbalance;
	break;

  case PSG_GSREG_LFOFREQ:
	value = lfofreq;
	break;

  case PSG_GSREG_LFOCTRL:
	value = lfoctrl;
	break;

  case PSG_GSREG_CH0_FREQ:
	value = channel[ch].frequency;
	break;

  case PSG_GSREG_CH0_CTRL:
	value = channel[ch].control;
	break;

  case PSG_GSREG_CH0_BALANCE:
	value = channel[ch].balance;
	break;

  case PSG_GSREG_CH0_WINDEX:
	value = channel[ch].waveform_index;
	break;

  case PSG_GSREG_CH0_SCACHE:
	value = channel[ch].dda;
	break;

  case PSG_GSREG_CH0_NCTRL:
	value = channel[ch].noisectrl;
	break;

  case PSG_GSREG_CH0_LFSR:
	value = channel[ch].lfsr & 0x7FFF;
	break;
 }
 return(value);
}


void PCE_PSG::SetRegister(const unsigned int id, const uint32 value)
{
 const int ch = (id >> 8) & 0xF;

 switch(id & 0xF0FF)
 {
  default: break;

  case PSG_GSREG_SELECT:
	select = value & 0x07;
	break;

  case PSG_GSREG_GBALANCE:
	globalbalance = value & 0xFF;
	break;

  case PSG_GSREG_LFOFREQ:
	lfofreq = value & 0xFF;
	break;

  case PSG_GSREG_LFOCTRL:
	lfoctrl = value & 0x83;
        RecalcFreqCache(0);
        RecalcUOFunc(0);
        RecalcFreqCache(1);
        RecalcUOFunc(1);
	break;

  case PSG_GSREG_CH0_FREQ:
	channel[ch].frequency = value & 0xFFF;
	RecalcFreqCache(ch);
	RecalcUOFunc(ch);
	break;

  case PSG_GSREG_CH0_CTRL:
	channel[ch].control = value & 0xFF;
	RecalcFreqCache(ch);
	RecalcUOFunc(ch);
	break;

  case PSG_GSREG_CH0_BALANCE:
	channel[ch].balance = value & 0xFF;
	break;

  case PSG_GSREG_CH0_WINDEX:
	channel[ch].waveform_index = value & 0x1F;
	break;

  case PSG_GSREG_CH0_SCACHE:
	channel[ch].dda = value & 0x1F;
	break;

  case PSG_GSREG_CH0_NCTRL:
	channel[ch].noisectrl = value & 0xFF;
        RecalcNoiseFreqCache(ch);
        RecalcUOFunc(ch);
	break;

  case PSG_GSREG_CH0_LFSR:
	channel[ch].lfsr = value & 0x7FFF;
	break;
 }
}

PCE_PSG::PCE_PSG(int32* hr_l, int32* hr_r, int want_revision)
{
	revision = want_revision;
	switch(revision)
	{
	 default:
		abort();
		break;

	 case REVISION_HUC6280:
		UpdateOutput_Accum = &PCE_PSG::UpdateOutput_Accum_HuC6280;
		break;

	 case REVISION_HUC6280A:
		UpdateOutput_Accum = &PCE_PSG::UpdateOutput_Accum_HuC6280A;
		break;
	}
	HRBufs[0] = hr_l;
	HRBufs[1] = hr_r;

	lastts = 0;
	for(int ch = 0; ch < 6; ch++)
	{
	 channel[ch].blip_prev_samp[0] = 0;
	 channel[ch].blip_prev_samp[1] = 0;
	 channel[ch].lastts = 0;
	}

	SetVolume(1.0);	// Will build dbtable in the process.
	Power(0);
}

PCE_PSG::~PCE_PSG()
{


}

int32 PCE_PSG::GetVL(const int chnum, const int lr)
{
     psg_channel *ch = &channel[chnum];

     const int gbal = 0x1F - scale_tab[(globalbalance >> (lr ? 0 : 4)) & 0xF];
     const int bal = 0x1F - scale_tab[(ch->balance >> (lr ? 0 : 4)) & 0xF];
     const int al = 0x1F - (ch->control & 0x1F);
     int vol_reduction;

     vol_reduction = gbal + bal + al;

     if(vol_reduction > 0x1F)
      vol_reduction = 0x1F;

     return(vol_reduction);
}

void PCE_PSG::Write(int32 timestamp, uint8 A, uint8 V)
{	
    A &= 0x0F;

    if(A == 0x00)
    {
     select = (V & 0x07);
     return;
    }

    Update(timestamp);

    psg_channel *ch = &channel[select];

    switch(A)
    {
	default: break;

        case 0x01: /* Global sound balance */
            globalbalance = V;
	    vol_pending = true;
            break;

        case 0x02: /* Channel frequency (LSB) */
	    if(select > 5) return; // no more than 6 channels, silly game.

            ch->frequency = (ch->frequency & 0x0F00) | V;
	    RecalcFreqCache(select);
	    RecalcUOFunc(select);
            break;

        case 0x03: /* Channel frequency (MSB) */
	    if(select > 5) return; // no more than 6 channels, silly game.

            ch->frequency = (ch->frequency & 0x00FF) | ((V & 0x0F) << 8);
	    RecalcFreqCache(select);
	    RecalcUOFunc(select);
            break;

        case 0x04: /* Channel enable, DDA, volume */
	    if(select > 5) return; // no more than 6 channels, silly game.

            if((ch->control & 0x40) && !(V & 0x40))
	    {
	     ch->waveform_index = 0;
             ch->dda = ch->waveform[ch->waveform_index];
	     ch->counter = ch->freq_cache;
	    }

	    if(!(ch->control & 0x80) && (V & 0x80)) 
	    {
	     if(!(V & 0x40))
	     {
	      ch->waveform_index = (ch->waveform_index + 1) & 0x1F;
	      ch->dda = ch->waveform[ch->waveform_index];
	     }
	    }

            ch->control = V;
	    RecalcFreqCache(select);
	    RecalcUOFunc(select);

	    vol_pending = true;
            break;

        case 0x05: /* Channel balance */
	    if(select > 5) return; // no more than 6 channels, silly game.
            ch->balance = V;

	    vol_pending = true;
            break;

        case 0x06: /* Channel waveform data */
            if(select > 5) return; // no more than 6 channels, silly game.
            V &= 0x1F;

            if(!(ch->control & 0x40))
	    {
	     ch->samp_accum -= ch->waveform[ch->waveform_index];
             ch->waveform[ch->waveform_index] = V;
	     ch->samp_accum += ch->waveform[ch->waveform_index];
	    }

            if((ch->control & 0xC0) == 0x00)
             ch->waveform_index = ((ch->waveform_index + 1) & 0x1F);

	    if(ch->control & 0x80)
	    {
	     // According to my tests(on SuperGrafx), writing to this channel
	     // will update the waveform value cache/latch regardless of DDA mode being enabled.
             ch->dda = V;
	    }
            break;

        case 0x07: /* Noise enable and frequency */
	    if(select > 5) return; // no more than 6 channels, silly game.
            if(select >= 4)
	    {
	     ch->noisectrl = V;
	     RecalcNoiseFreqCache(select);
	     RecalcUOFunc(select);
	    }
            break;

        case 0x08: /* LFO frequency */
            lfofreq = V & 0xFF;
            break;

        case 0x09: /* LFO trigger and control */
	    if(V & 0x80)
	    {
	     channel[1].waveform_index = 0;
	     channel[1].dda = channel[1].waveform[channel[1].waveform_index];
	     channel[1].counter = channel[1].freq_cache;
	    }
            lfoctrl = V;
	    RecalcFreqCache(0);
	    RecalcUOFunc(0);
	    RecalcFreqCache(1);
	    RecalcUOFunc(1);
            break;
    }
}

// Don't use INLINE, which has always_inline in it, due to gcc's inability to cope with the type of recursion
// used in this function.
void PCE_PSG::RunChannel(int chc, int32 timestamp, const bool LFO_On)
{
 psg_channel *ch = &channel[chc];
 int32 running_timestamp = ch->lastts;
 int32 run_time = timestamp - ch->lastts;

 ch->lastts = timestamp;

 if(!run_time)
  return;

 (this->*ch->UpdateOutput)(running_timestamp, ch);

 if(chc >= 4)
 {
  int32 freq = ch->noise_freq_cache;

  ch->noisecount -= run_time;

  if(&PCE_PSG::UpdateOutput_Noise == ch->UpdateOutput)
   while(ch->noisecount <= 0)
   {
    CLOCK_LFSR(ch->lfsr);
    UpdateOutput_Noise(timestamp + ch->noisecount, ch);
    ch->noisecount += freq;
   }
  else
   while(ch->noisecount <= 0)
   {
    CLOCK_LFSR(ch->lfsr);
    ch->noisecount += freq;
   }
 }

 // D7 of control is 0, don't clock the counter at all.
 // D7 of lfocontrol is 1(and chc == 1), don't clock the counter at all(not sure about this)
 // In DDA mode, don't clock the counter.
 // (Noise being enabled isn't handled here since AFAIK it doesn't disable clocking of the waveform portion, its sound just overrides the sound from
 //  the waveform portion when the noise enable bit is set, which is handled in our RecalcUOFunc).
 if(!(ch->control & 0x80) || (chc == 1 && (lfoctrl & 0x80)) || (ch->control & 0x40))
  return;

 ch->counter -= run_time;

 if(!LFO_On && ch->freq_cache <= FREQC7M_COT)
 {
  if(ch->counter <= 0)
  {
   const int32 inc_count = ((0 - ch->counter) / ch->freq_cache) + 1;

   ch->counter += inc_count * ch->freq_cache;

   ch->waveform_index = (ch->waveform_index + inc_count) & 0x1F;
   ch->dda = ch->waveform[ch->waveform_index];
  }
 }

 while(ch->counter <= 0)
 {
  ch->waveform_index = (ch->waveform_index + 1) & 0x1F;
  ch->dda = ch->waveform[ch->waveform_index];

  (this->*ch->UpdateOutput)(timestamp + ch->counter, ch);

  if(LFO_On)
  {
   RunChannel(1, timestamp + ch->counter, false);
   RecalcFreqCache(0);
   RecalcUOFunc(0);

   ch->counter += (ch->freq_cache <= FREQC7M_COT) ? FREQC7M_COT : ch->freq_cache;	// Not particularly accurate, but faster.
  }
  else
   ch->counter += ch->freq_cache;
 }
}

void PCE_PSG::UpdateSubLFO(int32 timestamp)
{
 for(int chc = 0; chc < 6; chc++)
  RunChannel(chc, timestamp, chc == 0);
}

void PCE_PSG::UpdateSubNonLFO(int32 timestamp)
{
 for(int chc = 0; chc < 6; chc++)
  RunChannel(chc, timestamp, false);
}

void PCE_PSG::Update(int32 timestamp)
{
 int32 run_time = timestamp - lastts;

 if(vol_pending && !vol_update_counter && !vol_update_which)
 {
  vol_update_counter = 1;
  vol_pending = false;
 }

 bool lfo_on = (bool)(lfoctrl & 0x03);

 if(lfo_on)
 {
  if(!(channel[1].control & 0x80) || (lfoctrl & 0x80))
  {
   lfo_on = 0;
   RecalcFreqCache(0);
   RecalcUOFunc(0);
  }
 }

 int32 clocks = run_time;
 int32 running_timestamp = lastts;

 while(clocks > 0)
 {
  int32 chunk_clocks = clocks;

  if(vol_update_counter > 0 && chunk_clocks > vol_update_counter)
   chunk_clocks = vol_update_counter;

  running_timestamp += chunk_clocks;
  clocks -= chunk_clocks;

  if(lfo_on)
   UpdateSubLFO(running_timestamp);
  else
   UpdateSubNonLFO(running_timestamp);

  if(vol_update_counter > 0)
  {
   vol_update_counter -= chunk_clocks;
   if(!vol_update_counter)
   {
    const int phase = vol_update_which & 1;
    const int lr = ((vol_update_which >> 1) & 1) ^ 1;
    const int chnum = vol_update_which >> 2;

    if(!phase)
    {
     if(chnum < 6)
     {
      vol_update_vllatch = GetVL(chnum, lr);
     }
     //last_read = running_timestamp;
    }
    else
    {
     if(chnum < 6)
     {
      channel[chnum].vl[lr] = vol_update_vllatch;
     }
     //last_apply = running_timestamp;
    }
    vol_update_which = (vol_update_which + 1) & 0x1F;

    if(vol_update_which)
     vol_update_counter = phase ? 1 : 255;
    else if(vol_pending)
    {
     vol_update_counter = phase ? 1 : 255;
     vol_pending = false;
    }
   }
  }

  lastts = running_timestamp;
 }
}

void PCE_PSG::ResetTS(int32 ts_base)
{
 lastts = ts_base;

 for(int chc = 0; chc < 6; chc++)
  channel[chc].lastts = ts_base;
}

void PCE_PSG::Power(const int32 timestamp)
{
 // Not sure about power-on values, these are mostly just intuitive guesses(with some laziness thrown in).
 if(timestamp != lastts)
  Update(timestamp);

 // Don't memset channel to 0, there's stuff like lastts and blip_prev_samp that shouldn't be altered on Power().

 select = 0;
 globalbalance = 0;
 lfofreq = 0;
 lfoctrl = 0;

 for(int ch = 0; ch < 6; ch++)
 {
  channel[ch].frequency = 0;
  channel[ch].control = 0x00;
  channel[ch].balance = 0;
  memset(channel[ch].waveform, 0, 32);
  channel[ch].samp_accum = 0;

  channel[ch].waveform_index = 0;
  channel[ch].dda = 0x00;
  channel[ch].noisectrl = 0x00;
  
  channel[ch].vl[0] = 0x1F;
  channel[ch].vl[1] = 0x1F;

  channel[ch].samp_accum = 0;

  RecalcFreqCache(ch);
  RecalcUOFunc(ch);

  channel[ch].counter = channel[ch].freq_cache;

  if(ch >= 4)
  {
   RecalcNoiseFreqCache(ch);
   channel[ch].noisecount = 1;
   channel[ch].lfsr = 1;
  }
 }

 vol_pending = false;
 vol_update_counter = 0;
 vol_update_which = 0;
}

int PCE_PSG::StateAction(StateMem *sm, int load, int data_only)
{
 int ret = 1;

 for(int ch = 0; ch < 6; ch++)
 {
  char tmpstr[5] = "SCHx";
  psg_channel *pt = &channel[ch];

  SFORMAT CH_StateRegs[] = 
  {
   SFVARN(pt->counter, "counter"),
   SFVARN(pt->frequency, "frequency"),
   SFVARN(pt->control, "control"),
   SFVARN(pt->balance, "balance"),
   SFARRAYN(pt->waveform, 32, "waveform"),
   SFVARN(pt->waveform_index, "waveform_index"),
   SFVARN(pt->dda, "dda"),
   SFVARN(pt->noisectrl, "noisectrl"),
   SFVARN(pt->noisecount, "noisecount"),
   SFVARN(pt->lfsr, "lfsr"),
   SFARRAY32N(pt->vl, 2, "vl"), // TODO
   SFEND
  };
  tmpstr[3] = '0' + ch;
  ret &= MDFNSS_StateAction(sm, load, data_only, CH_StateRegs, tmpstr, false);
 }

 SFORMAT PSG_StateRegs[] =
 {
  SFVAR(select),
  SFVAR(globalbalance),
  SFVAR(lfofreq),
  SFVAR(lfoctrl),

  SFVAR(vol_update_counter),
  SFVAR(vol_update_which),
  SFVAR(vol_pending),
  SFEND
 };
 
 ret &= MDFNSS_StateAction(sm, load, data_only, PSG_StateRegs, "PSG", false);

 if(load)
 {
  vol_update_which &= 0x1F;

  if(!channel[4].lfsr)
   channel[4].lfsr = 1;

  if(!channel[5].lfsr)
   channel[5].lfsr = 1;

  for(int ch = 0; ch < 6; ch++)
  {
   channel[ch].samp_accum = 0;
   for(int wi = 0; wi < 32; wi++)
   {
    channel[ch].waveform[wi] &= 0x1F;
    channel[ch].samp_accum += channel[ch].waveform[wi];
   }

   for(int lr = 0; lr < 2; lr++)
    channel[ch].vl[lr] &= 0x1F;

   if(!channel[ch].noisecount && ch >= 4)
    channel[ch].noisecount = 1;

   if(channel[ch].counter <= 0)
    channel[ch].counter = 1;

   if(ch >= 4)
    RecalcNoiseFreqCache(ch);
   RecalcFreqCache(ch);
   RecalcUOFunc(ch);
  }
 }
 return(ret); 
}
