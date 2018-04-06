//
//	EXtractoR
//		by Brendan Bolles <brendan@fnordware.com>
//
//	extract float channels
//
//	Part of the fnord OpenEXR tools
//		http://www.fnordware.com/OpenEXR/
//
//

#pragma once

#ifndef _CRYPTOMATTE_H_
#define _CRYPTOMATTE_H_


#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_ChannelSuites.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "fnord_SuiteHandler.h"

#ifdef MSWindows
	#include <Windows.h>
#else 
	#ifndef __MACH__
		#include <string.h>
	#endif
#endif	


// Versioning information 

#define NAME				"Cryptomatte"
#define DESCRIPTION			"Better ID Mattes"
#define RELEASE_DATE		__DATE__
#define AUTHOR				"Brendan Bolles"
#define COPYRIGHT			"(c) 2018 fnord"
#define WEBSITE				"www.fnordware.com"
#define	MAJOR_VERSION		1
#define	MINOR_VERSION		9
#define	BUG_VERSION			0
#define	STAGE_VERSION		PF_Stage_RELEASE
#define	BUILD_VERSION		0


enum {
	CRYPTO_INPUT = 0,
	CRYPTO_DATA,
	//CRYPTO_ACTION,
	CRYPTO_MATTE_ONLY,
	
	CRYPTO_NUM_PARAMS
};

enum {
	ARBITRARY_DATA_ID = 1,
	//ACTION_ID,
	MATTE_ID
};

/*
enum {
	ACTION_SELECT = 1,
	ACTION_ADD,
	ACTION_REMOVE,
	ACTION_NUM_OPTIONS = ACTION_REMOVE
};
#define ACTION_MENU_STR	"Select|Add|Remove"
*/

/*
enum {
	STATUS_UNKNOWN = 0,
	STATUS_NORMAL,
	STATUS_INDEX_CHANGE,
	STATUS_NOT_FOUND,
	STATUS_NO_CHANNELS,
	STATUS_ERROR
};
typedef A_char	Status;

typedef struct {
	Status			status;
	A_long			index; // in case of STATUS_INDEX_CHANGE
} ChannelStatus;

typedef struct {
	ChannelStatus	red;
	ChannelStatus	green;
	ChannelStatus	blue;
	ChannelStatus	alpha;
	ChannelStatus	compound;
} ExtractStatus;


enum {
	DO_EXTRACT = 1,
	DO_COPY,
	DO_FULL_ON,
	DO_FULL_OFF,
	DO_NOTHING
};
typedef A_char	ExtractAction;

typedef struct {
	ExtractAction	action;
	A_long			index; // 0-based index in the file
	char			reserved[27]; // total of 32 bytes up to here
	A_char			name[MAX_CHANNEL_NAME_LEN+1];
} ChannelData;

#define CURRENT_ARB_VERSION 1
*/

#define MAX_LAYER_NAME_LEN 63 // same as PF_CHANNEL_NAME_LEN

typedef struct {
	char		magic[4]; // "cry1"
	A_u_long	hash; // djb2 hash of everything after this, for quick comparison
	char		reserved[24]; // 32 bytes at this point
	char		layer[MAX_LAYER_NAME_LEN + 1];
	A_u_long	manifest_size; // including null character
	A_u_long	selection_size;
	char		data[4]; // manifest string + selection string 
} CryptomatteArbitraryData;


typedef struct {
	void		*context;
	A_Boolean	selectionChanged;
} CryptomatteSequenceData;

//#define ARB_REFCON			(void*)0xDEADBEAF

/*
// UI drawing constants
#define kUI_ARROW_MARGIN	20
#define kUI_RIGHT_MARGIN	5
#define kUI_TOP_MOVEDOWN	11
#define kUI_BOTTOM_MOVEUP	0


#ifdef MAC_ENV
#define kUI_CONTROL_DOWN	10
#define kUI_CONTROL_UP		10
#else
	#if PF_AE_PLUG_IN_VERSION >= PF_AE100_PLUG_IN_VERSION
	#define kUI_CONTROL_DOWN	10
	#define kUI_CONTROL_UP		10
	#else
	#define kUI_CONTROL_DOWN	0
	#define kUI_CONTROL_UP		0
	#endif
#endif
#define kUI_CONTROL_STEP	15


#define kUI_CONTROL_RIGHT	8
#define kUI_CONTROL_PROP_PADDING	16

#if PF_AE_PLUG_IN_VERSION >= PF_AE100_PLUG_IN_VERSION
#define kUI_TITLE_COLOR_SCALEDOWN	0.15
#else
#define kUI_TITLE_COLOR_SCALEDOWN	0.3
#endif

#define TITLE_COMP(COMP)	(65535 * kUI_TITLE_COLOR_SCALEDOWN) + ((COMP) * (1 - kUI_TITLE_COLOR_SCALEDOWN) )

#define DOWN_PLACE(NUM, OR)	(OR) + kUI_CONTROL_DOWN + (kUI_CONTROL_STEP * (NUM))
#define RIGHT_STATUS(OR)	(OR) + kUI_CONTROL_RIGHT
#define RIGHT_PROP(OR)		(OR) + kUI_CONTROL_RIGHT + kUI_CONTROL_PROP_PADDING


enum { //Info elements
	INFO_RED = 0,
	INFO_GREEN,
	INFO_BLUE,
	INFO_ALPHA,
	
	INFO_TOTAL_ITEMS
};
*/

#define kUI_CONTROL_HEIGHT	70
#define kUI_CONTROL_WIDTH	0

#ifdef __cplusplus

#include <string>
//#include <list>
#include <vector>
#include <set>
#include <map>

class CryptomatteException : public std::exception
{
  public:
	CryptomatteException(const std::string &what) throw() : _what(what) {}
	virtual ~CryptomatteException() throw() {}
	
	virtual const char* what() const throw() { return _what.c_str(); }

  private:
	std::string _what;
};


class CryptomatteContext
{
  public:
	CryptomatteContext(CryptomatteArbitraryData *arb);
	~CryptomatteContext();
	
	void SetSelection(CryptomatteArbitraryData *arb);
	
	void LoadLevels(PF_InData *in_data);
	
	bool Valid() const { return _levels.size() > 0; }
	
	float GetCoverage(int x, int y) const;
	
	std::set<std::string> GetItems(int x, int y) const;
	
	int Width() const;
	int Height() const;
	
	A_u_long Hash() const { return _hash; }
	
	static std::string searchReplace(const std::string &str, const std::string &search, const std::string &replace);
	static std::string deQuote(const std::string &s);
	static void quotedTokenize(const std::string &str, std::vector<std::string> &tokens, const std::string& delimiters = " ");

  private:
	A_u_long _hash;
	
	std::string _layer;
	std::map<std::string, A_u_long> _manifest;
	std::set<A_u_long> _selection;
	
	class Level
	{
	  public:
		Level(PF_InData *in_data, PF_ChannelRef &hash, PF_ChannelRef &coverage);
		Level(PF_InData *in_data, PF_ChannelRef &four, bool secondHalf);
		~Level();
		
		float GetCoverage(const std::set<A_u_long> &selection, int x, int y) const;
		
		A_u_long GetHash(int x, int y) const;
		
		inline int Width() const { return (_hash ? _hash->Width() : 0); }
		inline int Height() const { return (_hash ? _hash->Height() : 0); }
		
	  private:		
		class FloatBuffer
		{
		  public:
			FloatBuffer(PF_InData *in_data, char *origin, int width, int height, ptrdiff_t xStride, ptrdiff_t yStride);
			~FloatBuffer();
			
			inline float Get(int x, int y) const { return *((float *)_buf + (_width * y) + x); }
			
			inline int Width() const { return _width; }
			inline int Height() const { return _height; }
		
		  private:
			char *_buf;
			int _width;
			int _height;
		};
		
		FloatBuffer *_hash;
		FloatBuffer *_coverage;
	};
	
	std::vector<Level *> _levels;
	
	void CalculateNextNames(std::string &nextHashName, std::string &nextCoverageName);
	void CalculateNext4Name(std::string &fourName);
};

extern "C" {
#endif


// Prototypes

DllExport	PF_Err 
PluginMain (	
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra) ;


PF_Err 
DoDialog (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output );


PF_Err
HandleEvent ( 
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	PF_EventExtra	*extra );


PF_Err
ArbNewDefault( // needed by ParamSetup()
	PF_InData			*in_data,
	PF_OutData			*out_data,
	void				*refconPV,
	PF_ArbitraryH		*arbPH);

PF_Err 
HandleArbitrary(
	PF_InData			*in_data,
	PF_OutData			*out_data,
	PF_ParamDef			*params[],
	PF_LayerDef			*output,
	PF_ArbParamsExtra	*extra);

const char *
GetLayer(const CryptomatteArbitraryData *arb);

const char *
GetSelection(const CryptomatteArbitraryData *arb);

const char *
GetManifest(const CryptomatteArbitraryData *arb);

void
SetArb(PF_InData *in_data, PF_ArbitraryH *arbH, std::string layer, std::string selection, std::string manifest);

void
SetArbSelection(PF_InData *in_data, PF_ArbitraryH *arbH, std::string selection);




#if defined(MAC_ENV) && PF_AE_PLUG_IN_VERSION >= PF_AE100_PLUG_IN_VERSION
void SetMickeyCursor(); // love our Mickey cursor, but we need an Objectice-C call in Cocoa
#endif

#ifdef __cplusplus
}
#endif



#endif // _CRYPTOMATTE_H_
