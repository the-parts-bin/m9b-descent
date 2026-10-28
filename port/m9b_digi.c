/* Descent on the m9b SoC: digital sound effects and music. D1X's
 * arch/sdl/digi.c (the sound slots and the objects and positions sounds are
 * linked to) with the SDL audio callback replaced by the m9b mixer below.
 * The MIDI (HMP) music is pre-rendered on a PC (tools/make_music.sh) and
 * streamed from the SD card, as in ../../doom/port/sound_m9b.c. */
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "error.h"
#include "mono.h"
#include "fix.h"
#include "vecmat.h"
#include "gr.h" // needed for piggy.h
#include "piggy.h"
#include "digi.h"
#include "sounds.h"
#include "wall.h"
#include "newdemo.h"
#include "kconfig.h"
#include "altsound.h"
#include "m9b_map.h"

#define MAX_SOUND_SLOTS 32
#define MIN_VOLUME 10

#define SOF_USED			1		// Set if this sample is used
#define SOF_PLAYING			2		// Set if this sample is playing on a channel
#define SOF_LINK_TO_OBJ		4		// Sound is linked to a moving object. If object dies, then finishes play and quits.
#define SOF_LINK_TO_POS		8		// Sound is linked to segment, pos
#define SOF_PLAY_FOREVER	16		// Play forever (or until level is stopped), otherwise plays once

typedef struct sound_object {
	short		signature;		// A unique signature to this sound
	ubyte		flags;			// Used to tell if this slot is used and/or currently playing, and how long.
	fix		max_volume;		// Max volume that this sound is playing at
	fix		max_distance;	        // The max distance that this sound can be heard at...
	int		volume;			// Volume that this sound is playing at
	int 		pan;			// Pan value that this sound is playing at
	int		handle; 		// What handle this sound is playing on.  Valid only if SOF_PLAYING is set.
	short		soundnum;		// The sound number that is playing
	union {	
		struct {
			short		segnum; 			// Used if SOF_LINK_TO_POS field is used
			short		sidenum;
			vms_vector	position;
		}pos;
		struct {
			short		 objnum;			 // Used if SOF_LINK_TO_OBJ field is used
			short		 objsignature;
		}obj;
	}link;
} sound_object;
#define lp_segnum link.pos.segnum
#define lp_sidenum link.pos.sidenum
#define lp_position link.pos.position

#define lo_objnum link.obj.objnum
#define lo_objsignature link.obj.objsignature

#define MAX_SOUND_OBJECTS 16
sound_object SoundObjects[MAX_SOUND_OBJECTS];
short next_signature=0;

//added/changed on 980905 by adb to make sfx volume work, on 990221 by adb changed F1_0 to F1_0 / 2
#define SOUND_MAX_VOLUME (F1_0 / 2)

int digi_volume = SOUND_MAX_VOLUME;
//end edit by adb

int digi_lomem = 0;

static int digi_initialised = 0;

struct sound_slot {
 int soundno;
 int playing;   // Is there a sample playing on this channel?
 int looped;    // Play this sample looped?
 fix pan;       // 0 = far left, 1 = far right
 fix volume;    // 0 = nothing, 1 = fully on
 //changed on 980905 by adb from char * to unsigned char * 
 unsigned char *samples;
 //end changes by adb
 unsigned int length; // Length of the sample
 unsigned int position; // Position we are at at the moment.
} SoundSlots[MAX_SOUND_SLOTS];

static int digi_sounds_initialized = 0;

//added on 980905 by adb to add rotating/volume based sound kill system
static int digi_max_channels = 16;
static int next_handle = 0;
int SampleHandles[32];
void reset_sounds_on_channel(int channel);
//end edit by adb

void digi_reset_digi_sounds(void);

/* Audio: the m9b SoC's audio engine plays 48 kHz 16-bit stereo from two
 * 1 KB halves (256 frames) and interrupts as it moves on from one; the
 * interrupt (trap.c -> sound_irq()) mixes the next half at 12 kHz, each
 * result held for 4 frames (the samples are 11025 Hz), with each slot's
 * pan and volume as the SDL callback computed them, and hands it back
 * (see ../../doom/port/sound_m9b.c). */
#define IRQ_PENDING   (*(volatile unsigned int *)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile unsigned int *)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile unsigned int *)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile unsigned int *)(M9B_CTRL + 0x10u))
#define CLOCK_CNT     (*(volatile unsigned int *)(M9B_TIMING + 0xCu))
#define PSRAM_BASE    M9B_PSRAM_BASE
#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)
#define FRAMES        256
#define HOLD          4
#define MIX_RATE      12000
#define SAMPLE_RATE   11025

static unsigned int audio_buf[2][FRAMES] __attribute__((aligned(256)));
static unsigned int silence[FRAMES] __attribute__((aligned(256)));
unsigned int sound_cycles, sound_mixes;   /* for profiling */

static void audio_cache_clean(const void *p, unsigned int len)
{
 unsigned int a;
 for (a = (unsigned int)p & ~63u; a < (unsigned int)p + len; a += 64u)
  asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

static int mus_take(void);   /* below, with the rest of the music */
static volatile int mus_playing;

static void mix_half(int half)
{
 int acc_l[FRAMES / HOLD], acc_r[FRAMES / HOLD];
 int any = 0, i, music = mus_playing;
 struct sound_slot *sl;
 const unsigned int step = (SAMPLE_RATE << 16) / MIX_RATE;
 unsigned int *out = audio_buf[half];

 for (sl = SoundSlots; sl < SoundSlots + MAX_SOUND_SLOTS; sl++)
 {
  unsigned int pos, end;
  fix vl, vr;
  int x;
  if (!sl->playing) continue;
  if ((x = sl->pan) & 0x8000) { vl = 0x20000 - x * 2; vr = 0x10000; }
  else { vl = 0x10000; vr = x * 2; }
  vl = fixmul(vl, (x = sl->volume)) >> 8;   /* 8.8 */
  vr = fixmul(vr, x) >> 8;
  if (!any) { memset(acc_l, 0, sizeof(acc_l)); memset(acc_r, 0, sizeof(acc_r)); any = 1; }
  pos = sl->position << 16;
  end = sl->length << 16;
  for (i = 0; i < FRAMES / HOLD; i++, pos += step)
  {
   int v;
   if (pos >= end)
   {
    if (!sl->looped) { sl->playing = 0; break; }
    pos -= end;
   }
   v = (int)sl->samples[pos >> 16] - 0x80;
   acc_l[i] += v * vl;
   acc_r[i] += v * vr;
  }
  sl->position = pos >> 16;
 }
 if (!any && !music)
 {
  AUDIO_PTR[half] = (unsigned int)silence - PSRAM_BASE;
  return;
 }
 if (!any) { memset(acc_l, 0, sizeof(acc_l)); memset(acc_r, 0, sizeof(acc_r)); }
 /* Each effects value covers HOLD frames; the music (24 kHz) adds a sample
  * per 2 frames on top. */
 for (i = 0; i < FRAMES / HOLD; i++)
 {
  int k;
  for (k = 0; k < HOLD; k += 2)
  {
   int m = music ? mus_take() : 0;
   int l = acc_l[i] + m, r = acc_r[i] + m;
   unsigned int w;
   if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
   if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
   w = (unsigned short)l | (unsigned int)(unsigned short)r << 16;
   out[HOLD * i + k] = w; out[HOLD * i + k + 1] = w;
  }
 }
 audio_cache_clean(out, sizeof(audio_buf[0]));
 AUDIO_PTR[half] = (unsigned int)out - PSRAM_BASE;
 sound_mixes++;
}

void sound_irq(unsigned int pending)   /* from the trap handler */
{
 unsigned int t0;
 if (!digi_initialised || !(pending & (IRQ_AUDIO_A | IRQ_AUDIO_B))) return;
 t0 = CLOCK_CNT;
 if (pending & IRQ_AUDIO_A) mix_half(0);
 if (pending & IRQ_AUDIO_B) mix_half(1);
 sound_cycles += CLOCK_CNT - t0;
}

static void mus_fill(int max_chunks);
void m9b_digi_poll(void) { if (mus_playing) mus_fill(2); }   /* once a frame */

/* Initialise audio devices. */
int digi_init()
{
 unsigned int m;
 memset(SampleHandles, 255, sizeof(SampleHandles));
 memset(silence, 0, sizeof(silence));
 audio_cache_clean(silence, sizeof(silence));
 asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
 AUDIO_PTR[0] = (unsigned int)silence - PSRAM_BASE;
 AUDIO_PTR[1] = (unsigned int)silence - PSRAM_BASE;
 IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
 IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
 AUDIO_CTRL = 1u;
 if (m & 8u) asm volatile("csrsi mstatus, 8");
 digi_initialised = 1;
 return 0;
}

/* Toggle audio */
void digi_reset() { }

/* Shut down audio */
void digi_close()
{
 if (!digi_initialised) return;
 digi_initialised = 0;
 AUDIO_CTRL = 0u;
 IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
}

static int get_free_slot()
{
 int i;
 for (i=0; i<MAX_SOUND_SLOTS; i++)
 {
  if (!SoundSlots[i].playing) return i;
 }
 return -1;
}

int digi_start_sound(int soundnum, fix volume, fix pan)
{
 int ntries;
 int slot;

 if (!digi_initialised) return -1;

 //added on 980905 by adb from original source to add sound kill system
 // play at most digi_max_channel samples, if possible kill sample with low volume
 ntries = 0;

TryNextChannel:
 if ( (SampleHandles[next_handle] >= 0) && (SoundSlots[SampleHandles[next_handle]].playing)  )
 {
  if ( (SoundSlots[SampleHandles[next_handle]].volume > digi_volume) && (ntries<digi_max_channels) )
  {
   //mprintf(( 0, "Not stopping loud sound %d.\n", next_handle ));
   next_handle++;
   if ( next_handle >= digi_max_channels )
    next_handle = 0;
   ntries++;
   goto TryNextChannel;
  }
  //mprintf(( 0, "[SS:%d]", next_handle ));
  SoundSlots[SampleHandles[next_handle]].playing = 0;
  SampleHandles[next_handle] = -1;
 }
 //end edit by adb

 slot = get_free_slot();
 if (slot<0) return -1;

 SoundSlots[slot].soundno = soundnum;
 SoundSlots[slot].samples = Sounddat(soundnum)->data;
 SoundSlots[slot].length = Sounddat(soundnum)->length;
 SoundSlots[slot].volume = fixmul(digi_volume, volume);
 SoundSlots[slot].pan = pan;
 SoundSlots[slot].position = 0;
 SoundSlots[slot].looped = 0;
 SoundSlots[slot].playing = 1;

 //added on 980905 by adb to add sound kill system from original sos digi.c
 reset_sounds_on_channel(slot);
 SampleHandles[next_handle] = slot;
 next_handle++;
 if ( next_handle >= digi_max_channels )
  next_handle = 0;
 //end edit by adb

 return slot;
}

 //added on 980905 by adb to add sound kill system from original sos digi.c
void reset_sounds_on_channel( int channel )
{
 int i;

 for (i=0; i<digi_max_channels; i++)
  if (SampleHandles[i] == channel)
   SampleHandles[i] = -1;
}
//end edit by adb

int digi_start_sound_object(int obj)
{
 int slot;

 if (!digi_initialised) return -1;
 slot = get_free_slot();

 if (slot<0) return -1;


 SoundSlots[slot].soundno = SoundObjects[obj].soundnum;
 SoundSlots[slot].samples = Sounddat(SoundObjects[obj].soundnum)->data;
 SoundSlots[slot].length = Sounddat(SoundObjects[obj].soundnum)->length;
 SoundSlots[slot].volume = fixmul(digi_volume, SoundObjects[obj].volume);
 SoundSlots[slot].pan = SoundObjects[obj].pan;
 SoundSlots[slot].position = 0;
 SoundSlots[slot].looped = (SoundObjects[obj].flags & SOF_PLAY_FOREVER);
 SoundSlots[slot].playing = 1;

 SoundObjects[obj].signature = next_signature++;
 SoundObjects[obj].handle = slot;

 SoundObjects[obj].flags |= SOF_PLAYING;
 //added on 980905 by adb to add sound kill system from original sos digi.c
 reset_sounds_on_channel(slot);
 //end edit by adb
 
 return 0;
}


// Play the given sound number.
// Volume is max at F1_0.
void digi_play_sample( int soundno, fix max_volume )
{
#ifdef NEWDEMO
	if ( Newdemo_state == ND_STATE_RECORDING )
		newdemo_record_sound( soundno );
#endif
	if (!digi_initialised) return;

        if (digi_xlat_sound(soundno) < 0 ) return;

	digi_start_sound(soundno, max_volume, F0_5);
}

// Play the given sound number. If the sound is already playing,
// restart it.
void digi_play_sample_once( int soundno, fix max_volume )
{
	int i;

#ifdef NEWDEMO
	if ( Newdemo_state == ND_STATE_RECORDING )
		newdemo_record_sound( soundno );
#endif

	if (!digi_initialised) return;

        if (digi_xlat_sound(soundno) < 0 ) return;

        for (i=0; i < MAX_SOUND_SLOTS; i++)
          if (SoundSlots[i].soundno == soundno)
            SoundSlots[i].playing = 0;
	digi_start_sound(soundno, max_volume, F0_5);

}

void digi_play_sample_3d( int soundno, int angle, int volume, int no_dups ) // Volume from 0-0x7fff
{
	no_dups = 1;

#ifdef NEWDEMO
	if ( Newdemo_state == ND_STATE_RECORDING )		{
		if ( no_dups )
			newdemo_record_sound_3d_once( soundno, angle, volume );
		else
			newdemo_record_sound_3d( soundno, angle, volume );
	}
#endif
	if (!digi_initialised) return;
        if (digi_xlat_sound(soundno) < 0 ) return;

	if (volume < MIN_VOLUME ) return;
	digi_start_sound(soundno, volume, angle);
}

void digi_get_sound_loc( vms_matrix * listener, vms_vector * listener_pos, int listener_seg, vms_vector * sound_pos, int sound_seg, fix max_volume, int *volume, int *pan, fix max_distance )
{	  
	vms_vector	vector_to_sound;
	fix angle_from_ear, cosang,sinang;
	fix distance;
	fix path_distance;

	*volume = 0;
	*pan = 0;

	max_distance = (max_distance*5)/4;		// Make all sounds travel 1.25 times as far.

	//	Warning: Made the vm_vec_normalized_dir be vm_vec_normalized_dir_quick and got illegal values to acos in the fang computation.
	distance = vm_vec_normalized_dir_quick( &vector_to_sound, sound_pos, listener_pos );
		
	if (distance < max_distance )	{
		int num_search_segs = f2i(max_distance/20);
		if ( num_search_segs < 1 ) num_search_segs = 1;

		path_distance = find_connected_distance(listener_pos, listener_seg, sound_pos, sound_seg, num_search_segs, WID_RENDPAST_FLAG );
		if ( path_distance > -1 )	{
			*volume = max_volume - fixdiv(path_distance,max_distance);
			//mprintf( (0, "Sound path distance %.2f, volume is %d / %d\n", f2fl(distance), *volume, max_volume ));
			if (*volume > 0 )	{
				angle_from_ear = vm_vec_delta_ang_norm(&listener->rvec,&vector_to_sound,&listener->uvec);
				fix_sincos(angle_from_ear,&sinang,&cosang);
				//mprintf( (0, "volume is %.2f\n", f2fl(*volume) ));
				if (Config_channels_reversed) cosang *= -1;
				*pan = (cosang + F1_0)/2;
			} else {
				*volume = 0;
			}
		}
	}																					  
}

int digi_link_sound_to_object2( int soundnum, short objnum, int forever, fix max_volume, fix  max_distance )
{
	int i,volume,pan;
	object * objp;

	if ( max_volume < 0 ) return -1;
//	if ( max_volume > F1_0 ) max_volume = F1_0;

	if (!digi_initialised) return -1;
	if (soundnum < 0 ) return -1;
        if (Sounddat(soundnum)->data==NULL) {
		Int3();
		return -1;
	}
	if ((objnum<0)||(objnum>Highest_object_index))
		return -1;

	if ( !forever )	{
		// Hack to keep sounds from building up...
		digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, &Objects[objnum].pos, Objects[objnum].segnum, max_volume,&volume, &pan, max_distance );
                digi_play_sample_3d(soundnum, pan, volume, 0 );
		return -1;
	}

       	for (i=0; i<MAX_SOUND_OBJECTS; i++ )
        	if (SoundObjects[i].flags==0)
	           break;

	if (i==MAX_SOUND_OBJECTS) {
		mprintf((1, "Too many sound objects!\n" ));
		return -1;
	}

	SoundObjects[i].signature=next_signature++;
	SoundObjects[i].flags = SOF_USED | SOF_LINK_TO_OBJ;
	if ( forever )
		SoundObjects[i].flags |= SOF_PLAY_FOREVER;
	SoundObjects[i].lo_objnum = objnum;
	SoundObjects[i].lo_objsignature = Objects[objnum].signature;
	SoundObjects[i].max_volume = max_volume;
	SoundObjects[i].max_distance = max_distance;
	SoundObjects[i].volume = 0;
	SoundObjects[i].pan = 0;
	SoundObjects[i].soundnum = soundnum;

	objp = &Objects[SoundObjects[i].lo_objnum];
	digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
                       &objp->pos, objp->segnum, SoundObjects[i].max_volume,
                       &SoundObjects[i].volume, &SoundObjects[i].pan, SoundObjects[i].max_distance );

	if (!forever || SoundObjects[i].volume >= MIN_VOLUME)
	       digi_start_sound_object(i);

	return SoundObjects[i].signature;
}

int digi_link_sound_to_object( int soundnum, short objnum, int forever, fix max_volume )
{ return digi_link_sound_to_object2( soundnum, objnum, forever, max_volume, 256*F1_0); }

int digi_link_sound_to_pos2( int soundnum, short segnum, short sidenum, vms_vector * pos, int forever, fix max_volume, fix max_distance )
{
	int i, volume, pan;

	if ( max_volume < 0 ) return -1;
//	if ( max_volume > F1_0 ) max_volume = F1_0;

	if (!digi_initialised) return -1;
        if (digi_xlat_sound(soundnum) < 0 ) return -1;
        if (Sounddat(soundnum)->data==NULL) {
		Int3();
		return -1;
	}

	if ((segnum<0)||(segnum>Highest_segment_index))
		return -1;

	if ( !forever )	{
		// Hack to keep sounds from building up...
		digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, pos, segnum, max_volume, &volume, &pan, max_distance );
                digi_play_sample_3d( soundnum, pan, volume, 0 );
		return -1;
	}

	for (i=0; i<MAX_SOUND_OBJECTS; i++ )
		if (SoundObjects[i].flags==0)
			break;
	
	if (i==MAX_SOUND_OBJECTS) {
		mprintf((1, "Too many sound objects!\n" ));
		return -1;
	}


	SoundObjects[i].signature=next_signature++;
	SoundObjects[i].flags = SOF_USED | SOF_LINK_TO_POS;
	if ( forever )
		SoundObjects[i].flags |= SOF_PLAY_FOREVER;
	SoundObjects[i].lp_segnum = segnum;
	SoundObjects[i].lp_sidenum = sidenum;
	SoundObjects[i].lp_position = *pos;
	SoundObjects[i].soundnum = soundnum;
	SoundObjects[i].max_volume = max_volume;
	SoundObjects[i].max_distance = max_distance;
	SoundObjects[i].volume = 0;
	SoundObjects[i].pan = 0;
	digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
					   &SoundObjects[i].lp_position, SoundObjects[i].lp_segnum,
					   SoundObjects[i].max_volume,
                       &SoundObjects[i].volume, &SoundObjects[i].pan, SoundObjects[i].max_distance );
	
	if (!forever || SoundObjects[i].volume >= MIN_VOLUME)
		digi_start_sound_object(i);

	return SoundObjects[i].signature;
}

int digi_link_sound_to_pos( int soundnum, short segnum, short sidenum, vms_vector * pos, int forever, fix max_volume )
{
	return digi_link_sound_to_pos2( soundnum, segnum, sidenum, pos, forever, max_volume, F1_0 * 256 );
}

void digi_kill_sound_linked_to_segment( int segnum, int sidenum, int soundnum )
{
	int i,killed;

	if (!digi_initialised) return;

	killed = 0;

	for (i=0; i<MAX_SOUND_OBJECTS; i++ )	{
		if ( (SoundObjects[i].flags & SOF_USED) && (SoundObjects[i].flags & SOF_LINK_TO_POS) )	{
			if ((SoundObjects[i].lp_segnum == segnum) && (SoundObjects[i].soundnum==soundnum ) && (SoundObjects[i].lp_sidenum==sidenum) ) {
				if ( SoundObjects[i].flags & SOF_PLAYING )	{
				        SoundSlots[SoundObjects[i].handle].playing = 0;
				}
				SoundObjects[i].flags = 0;	// Mark as dead, so some other sound can use this sound
				killed++;
			}
		}
	}
	// If this assert happens, it means that there were 2 sounds
	// that got deleted. Weird, get John.
	if ( killed > 1 )	{
		mprintf( (1, "ERROR: More than 1 sounds were deleted from seg %d\n", segnum ));
	}
}

void digi_kill_sound_linked_to_object( int objnum )
{
	int i,killed;

	if (!digi_initialised) return;

	killed = 0;

	for (i=0; i<MAX_SOUND_OBJECTS; i++ )	{
		if ( (SoundObjects[i].flags & SOF_USED) && (SoundObjects[i].flags & SOF_LINK_TO_OBJ ) )	{
			if (SoundObjects[i].lo_objnum == objnum)   {
				if ( SoundObjects[i].flags & SOF_PLAYING )	{
                                     SoundSlots[SoundObjects[i].handle].playing = 0;
				}
				SoundObjects[i].flags = 0;	// Mark as dead, so some other sound can use this sound
				killed++;
			}
		}
	}
	// If this assert happens, it means that there were 2 sounds
	// that got deleted. Weird, get John.
	if ( killed > 1 )	{
		mprintf( (1, "ERROR: More than 1 sounds were deleted from object %d\n", objnum ));
	}
}

void digi_sync_sounds()
{
	int i;
	int oldvolume, oldpan;

	if (!digi_initialised) return;

	for (i=0; i<MAX_SOUND_OBJECTS; i++ )	{
		if ( SoundObjects[i].flags & SOF_USED )	{
			oldvolume = SoundObjects[i].volume;
			oldpan = SoundObjects[i].pan;

			if ( !(SoundObjects[i].flags & SOF_PLAY_FOREVER) )	{
			 	// Check if its done.
				if (SoundObjects[i].flags & SOF_PLAYING) {
					if (!SoundSlots[SoundObjects[i].handle].playing) {
						SoundObjects[i].flags = 0;	// Mark as dead, so some other sound can use this sound
						continue;		// Go on to next sound...
					}
				}
			}			
		
			if ( SoundObjects[i].flags & SOF_LINK_TO_POS )	{
				digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
								&SoundObjects[i].lp_position, SoundObjects[i].lp_segnum,
								SoundObjects[i].max_volume,
                                &SoundObjects[i].volume, &SoundObjects[i].pan, SoundObjects[i].max_distance );

			} else if ( SoundObjects[i].flags & SOF_LINK_TO_OBJ )	{
				object * objp;
	
				objp = &Objects[SoundObjects[i].lo_objnum];
		
				if ((objp->type==OBJ_NONE) || (objp->signature!=SoundObjects[i].lo_objsignature))  {
					// The object that this is linked to is dead, so just end this sound if it is looping.
					if ( (SoundObjects[i].flags & SOF_PLAYING)  && (SoundObjects[i].flags & SOF_PLAY_FOREVER))	{
					     SoundSlots[SoundObjects[i].handle].playing = 0;
					}
					SoundObjects[i].flags = 0;	// Mark as dead, so some other sound can use this sound
					continue;		// Go on to next sound...
				} else {
					digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
	                                &objp->pos, objp->segnum, SoundObjects[i].max_volume,
                                   &SoundObjects[i].volume, &SoundObjects[i].pan, SoundObjects[i].max_distance );
				}
			}
			 
			if (oldvolume != SoundObjects[i].volume) 	{
				if ( SoundObjects[i].volume < MIN_VOLUME || Function_mode == FMODE_MENU)	 {
					// Sound is too far away, so stop it from playing.
					if ((SoundObjects[i].flags & SOF_PLAYING)&&(SoundObjects[i].flags & SOF_PLAY_FOREVER))	{
                                        	SoundSlots[SoundObjects[i].handle].playing = 0;
						SoundObjects[i].flags &= ~SOF_PLAYING;		// Mark sound as not playing
					}
				} else {
					if (!(SoundObjects[i].flags & SOF_PLAYING))	{
						digi_start_sound_object(i);
					} else {
					        SoundSlots[SoundObjects[i].handle].volume = fixmuldiv(SoundObjects[i].volume,digi_volume,F1_0);
					}
				}
			}
				
			if (oldpan != SoundObjects[i].pan) 	{
				if (SoundObjects[i].flags & SOF_PLAYING)
                                        SoundSlots[SoundObjects[i].handle].pan = SoundObjects[i].pan;
			}
		}
	}
}

void digi_init_sounds()
{
	int i;

	if (!digi_initialised) return;

	digi_reset_digi_sounds();

	for (i=0; i<MAX_SOUND_OBJECTS; i++ )	{
		if (digi_sounds_initialized) {
			if ( SoundObjects[i].flags & SOF_PLAYING )	{
			        SoundSlots[SoundObjects[i].handle].playing=0;
			}
		}
		SoundObjects[i].flags = 0;	// Mark as dead, so some other sound can use this sound
	}
	digi_sounds_initialized = 1;
}

//added on 980905 by adb from original source to make sfx volume work
void digi_set_digi_volume( int dvolume )
{
	dvolume = fixmuldiv( dvolume, SOUND_MAX_VOLUME, 0x7fff);
	if ( dvolume > SOUND_MAX_VOLUME )
		digi_volume = SOUND_MAX_VOLUME;
	else if ( dvolume < 0 )
		digi_volume = 0;
	else
		digi_volume = dvolume;

	if ( !digi_initialised ) return;

	digi_sync_sounds();
}
//end edit by adb

void digi_set_volume( int dvolume, int mvolume ) {
	digi_set_digi_volume(dvolume);
	digi_set_midi_volume(mvolume);
}

int digi_is_sound_playing(int soundno)
{
	int i;

	for (i = 0; i < MAX_SOUND_SLOTS; i++)
		  //changed on 980905 by adb: added SoundSlots[i].playing &&
		  if (SoundSlots[i].playing && SoundSlots[i].soundno == soundno)
		  //end changes by adb
			return 1;
	return 0;
}


void digi_pause_all() { }
void digi_resume_all() { }
void digi_stop_all() {
	int i;
	// ... Ano. The lack of this was causing ambient sounds to crash.
	// fixed, added digi_stop_all 07/19/01 - bluecow
	
	for (i=0; i<MAX_SOUND_OBJECTS; i++ )	{
		if ( SoundObjects[i].flags & SOF_USED )	{
			SoundSlots[SoundObjects[i].handle].playing = 0;
			SoundObjects[i].flags = 0;
		}
	}
}

 //added on 980905 by adb to make sound channel setting work
void digi_set_max_channels(int n) { 
	digi_max_channels	= n;

	if ( digi_max_channels < 1 ) 
		digi_max_channels = 1;
	if ( digi_max_channels > (MAX_SOUND_SLOTS-MAX_SOUND_OBJECTS) ) 
		digi_max_channels = (MAX_SOUND_SLOTS-MAX_SOUND_OBJECTS);

	if ( !digi_initialised ) return;

	digi_reset_digi_sounds();
}

int digi_get_max_channels() { 
	return digi_max_channels; 
}
// end edit by adb

void digi_reset_digi_sounds() {
 int i;

 for (i=0; i< MAX_SOUND_SLOTS; i++)
  SoundSlots[i].playing=0;
 
 //added on 980905 by adb to reset sound kill system
 memset(SampleHandles, 255, sizeof(SampleHandles));
 next_handle = 0;
 //end edit by adb
}


// MIDI stuff follows.
//added/killed on 11/25/98 by Matthew Mueller
//void digi_set_midi_volume( int mvolume ) { }
//void digi_play_midi_song( char * filename, char * melodic_bank, char * drum_bank, int loop ) {}
//void digi_stop_current_song()
//{
//#ifdef HMIPLAY
//        char buf[10];
//    
//        sprintf(buf,"s");
//        send_ipc(buf);
//#endif
//}
//end this section kill - MM

/* Music: the HMP songs rendered on a PC (tools/make_music.sh) to
 * DESCENT/MUSIC/<NAME>.WAV (GAME0.WAV for game0.hmp), 24 kHz 16-bit mono.
 * The main loop (m9b_digi_poll, once a frame) keeps a 256 KB ring (5.5 s,
 * more than a level load) filled from the card; the audio interrupt takes
 * a sample per 2 output frames from it. A song without a file is silent.
 * The volume is D1X's MIDI volume, 0..128 (the menu's 0..8 times 16): the
 * default, 64, plays the render as it is (normalized to about -18 LUFS,
 * peaks at most -1 dBFS), louder settings boost it, clipped. */
#define MUS_RATE      24000u
#define MUS_RING      (128u * 1024u)             /* samples */
#define MUS_CHUNK     (8u * 1024u)               /* samples per read (16 KB) */

static short mus_ring[MUS_RING] __attribute__((aligned(64)));
static volatile unsigned int mus_r, mus_w;       /* read by the interrupt, written by the main loop */
static int mus_fd = -1, mus_loop, mus_eof;
static unsigned int mus_data_start, mus_data_end;
static int mus_volume = 64;

/* The interrupt's part: the next sample, scaled (0 when the ring is empty). */
static int mus_take(void)
{
 int v;
 if (mus_r == mus_w) return 0;
 v = (mus_ring[mus_r++ & (MUS_RING - 1u)] * mus_volume) >> 6;
 if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
 return v;
}

static void mus_close(void)
{
 mus_playing = 0;
 if (mus_fd >= 0) close(mus_fd);
 mus_fd = -1;
}

static unsigned int le32(const unsigned char *p) { return p[0] | p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24; }

/* Opens a WAV file: 0 if it is 24 kHz 16-bit mono PCM, positioned at its samples. */
static int mus_open_wav(const char *path)
{
 unsigned int pos = 12;
 int fmt_ok = 0;
 unsigned char h[16];
 mus_fd = open(path, O_RDONLY);
 if (mus_fd < 0) return -1;
 if (read(mus_fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
 for (;;)   /* chunks: "fmt " then "data" (others skipped) */
 {
  unsigned int size;
  if (lseek(mus_fd, pos, SEEK_SET) != (off_t)pos || read(mus_fd, h, 8) != 8) return -1;
  size = le32(h + 4);
  if (!memcmp(h, "fmt ", 4))
  {
   if (read(mus_fd, h, 16) != 16) return -1;
   fmt_ok = h[0] == 1 && h[1] == 0 && h[2] == 1 && le32(h + 4) == MUS_RATE && h[14] == 16;
  }
  else if (!memcmp(h, "data", 4))
  {
   if (!fmt_ok) return -1;
   mus_data_start = pos + 8u;
   mus_data_end = mus_data_start + (size & ~1u);
   return lseek(mus_fd, mus_data_start, SEEK_SET) == (off_t)mus_data_start ? 0 : -1;
  }
  pos += 8u + ((size + 1u) & ~1u);
 }
}

/* Tops up the ring with up to max_chunks reads of at most 16 KB (the music
 * takes 48 KB/s), so a new song fills it over a few frames. */
static void mus_fill(int max_chunks)
{
 if (mus_fd < 0 || mus_eof) return;
 while (max_chunks-- > 0)
 {
  unsigned int w = mus_w, space = MUS_RING - (w - mus_r);
  unsigned int at = w & (MUS_RING - 1u), n = MUS_RING - at, left;
  off_t pos;
  if (n > MUS_CHUNK) n = MUS_CHUNK;
  if (space < n) return;
  pos = lseek(mus_fd, 0, SEEK_CUR);
  left = (mus_data_end - (unsigned int)pos) / 2u;
  if (n > left) n = left;
  if (n)
  {
   ssize_t got = read(mus_fd, &mus_ring[at], n * 2u);
   if (got <= 0) { mus_eof = 1; return; }
   mus_w = w + (unsigned int)got / 2u;
   continue;
  }
  if (!mus_loop || lseek(mus_fd, mus_data_start, SEEK_SET) < 0) { mus_eof = 1; return; }
  max_chunks++;   /* the seek read nothing */
 }
}

void digi_play_midi_song( char * filename, char * melodic_bank, char * drum_bank, int loop )
{
 char path[48], name[16];
 int i;
 mus_close();
 if (!filename || !filename[0]) return;
 for (i = 0; filename[i] && filename[i] != '.' && i < 15; i++)
  name[i] = (char)toupper((unsigned char)filename[i]);
 name[i] = 0;
 snprintf(path, sizeof(path), "DESCENT/MUSIC/%s.WAV", name);
 if (mus_open_wav(path) < 0)
 {
  printf("music: no %s\n", path);
  mus_close();
  return;
 }
 mus_loop = loop;
 mus_eof = 0;
 mus_r = mus_w = 0;
 mus_fill(2);
 mus_playing = 1;
}

void digi_set_midi_volume( int mvolume )
{
 mus_volume = mvolume < 0 ? 0 : mvolume > 127 ? 127 : mvolume;
}
