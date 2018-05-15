//
//	Cryptomatte AE plug-in
//		by Brendan Bolles <brendan@fnordware.com>
//
//
//	Part of ProEXR
//		http://www.fnordware.com/ProEXR
//
//

#include "Cryptomatte_AE.h"

#include "Cryptomatte_AE_Dialog.h"

#include "picojson.h"

#include <assert.h>

#include <float.h>

#include <sstream>
#include <iomanip>


//#include "MurmurHash3.h"
void MurmurHash3_x86_32  ( const void * key, int len, unsigned int seed, void * out );


//using namespace std;

class ErrThrower : public std::exception
{
  public:
	ErrThrower(A_Err err = A_Err_NONE) throw() : _err(err) {}
	ErrThrower(const ErrThrower &other) throw() : _err(other._err) {}
	virtual ~ErrThrower() throw() {}

	ErrThrower & operator = (A_Err err)
	{
		_err = err;
		
		if(_err != A_Err_NONE)
			throw *this;
		
		return *this;
	}

	A_Err err() const { return _err; }
	
	virtual const char* what() const throw() { return "AE Error"; }
	
  private:
	A_Err _err;
};


#ifndef NDEBUG
static int gNumContexts = 0;
#endif

CryptomatteContext::CryptomatteContext(CryptomatteArbitraryData *arb) :
	_manifestHash(0),
	_selectionHash(0),
	_buffer(NULL)
{
	if(arb == NULL)
		throw CryptomatteException("no arb");
	
	Update(arb);
	
	_downsampleX.num = _downsampleX.den = 0;
	_downsampleY.num = _downsampleY.den = 0;
	_currentTime = -1;

#ifndef NDEBUG
	gNumContexts++;
#endif
}


CryptomatteContext::~CryptomatteContext()
{
	delete _buffer;

#ifndef NDEBUG
	gNumContexts--;
#endif
}


void
CryptomatteContext::Update(CryptomatteArbitraryData *arb)
{
	if(_layer != GetLayer(arb))
		_layer = GetLayer(arb);
	
	if(_manifestHash != arb->manifest_hash)
	{
		_manifestHash = arb->manifest_hash;

		_manifest.clear();

		picojson::value manifestObj;
		picojson::parse(manifestObj, GetManifest(arb));
		
		if( manifestObj.is<picojson::object>() )
		{
			const picojson::object &object = manifestObj.get<picojson::object>();
			
			for(picojson::object::const_iterator i = object.begin(); i != object.end(); ++i)
			{
				const std::string &name = i->first;
				const picojson::value &value = i->second;
				
				if( value.is<std::string>() )
				{
					Hash literal_val;
					if(GetHashIfLiteral("<" + value.get<std::string>() + ">", literal_val))
						_manifest[name] = literal_val;
				}
			}
		}
	}

	if(_selectionHash != arb->selection_hash)
	{
		_selectionHash = arb->selection_hash;
		_selection = GetSelection(arb);

		_float_selection.clear();

		try
		{			
			if(!_selection.empty())
			{
				std::vector<std::string> tokens;
				quotedTokenize(_selection, tokens, ", ");
				
				for(std::vector<std::string>::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
				{
					const std::string val = deQuote(*i);

					Hash literal_val;

					if( _manifest.count(val) )
					{
						const Hash &hash = _manifest[val];
						
						_float_selection.insert( HashToFloatHash(hash) );
					}
					else if(GetHashIfLiteral(val, literal_val))
					{
						_float_selection.insert(HashToFloatHash(literal_val));
					}
					else if(val.size())
					{	
						_float_selection.insert(HashToFloatHash(HashName(val)));
					}
				}
			}
		}
		catch(...) {}
	}
}


void
CryptomatteContext::LoadLevels(PF_InData *in_data)
{
	if(_buffer != NULL)
	{
		delete _buffer;
		
		_buffer = NULL;
	}
	
	
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	PF_ChannelSuite *cs = suites.PFChannelSuite();
	
	A_long num_channels = 0;
	cs->PF_GetLayerChannelCount(in_data->effect_ref, CRYPTO_INPUT, &num_channels);
	
	if(_layer.empty() && num_channels > 0)
	{
		// search channels to see if we have a layer even though it hasn't been named in the ArbData
		for(int i=0; i < num_channels && _layer.empty(); i++)
		{
			PF_Boolean found;
			PF_ChannelRef channelRef;
			PF_ChannelDesc channelDesc;
			
			cs->PF_GetLayerChannelIndexedRefAndDesc(in_data->effect_ref,
													CRYPTO_INPUT,
													i,
													&found,
													&channelRef,
													&channelDesc);
			
			if(found && channelDesc.channel_type && channelDesc.data_type == PF_DataType_FLOAT &&
				channelDesc.dimension == 1)
			{
				const std::string chan_name = channelDesc.name;
				
				const std::string::size_type dot_pos = chan_name.rfind(".");
				
				if(dot_pos != std::string::npos && dot_pos > 3)
				{
					const std::string color_name = chan_name.substr(dot_pos + 1);
					const std::string layer_num = chan_name.substr(dot_pos - 2, 2);
					
					if((color_name == "R" || color_name == "r" || color_name == "red") && layer_num == "00")
					{
						_layer = chan_name.substr(0, dot_pos - 2);
					}
				}
			}
		}
	}

	if(!_layer.empty() && num_channels > 0)
	{
		unsigned int numLevels = 0;
		std::vector<PF_ChannelRef> channelRefs;
	
		// first we try to find 4-channel names
		for(int s = NAMING_BEGIN; s <= NAMING_END; s++)
		{
			std::string nextFourName;
			CalculateNext4Name(nextFourName, (NamingStyle)s, numLevels);
			
			for(int i=0; i < num_channels; i++)
			{
				PF_Boolean found;
				PF_ChannelRef channelRef;
				PF_ChannelDesc channelDesc;
				
				cs->PF_GetLayerChannelIndexedRefAndDesc(in_data->effect_ref,
														CRYPTO_INPUT,
														i,
														&found,
														&channelRef,
														&channelDesc);
				
				if(found && channelDesc.channel_type && channelDesc.data_type == PF_DataType_FLOAT &&
					channelDesc.dimension == 4 && channelDesc.name == nextFourName)
				{
					numLevels += 2;
					
					channelRefs.push_back(channelRef);
					
					CalculateNext4Name(nextFourName, (NamingStyle)s, numLevels);
					
					i = 0; // start over
				}
			}
		}
		
		
		for(int s = NAMING_BEGIN; s <= NAMING_END; s++)
		{
			std::string nextHashName, nextCoverageName;
			CalculateNextNames(nextHashName, nextCoverageName, (NamingStyle)s, numLevels);
			
			PF_ChannelRef hash, coverage;
			bool foundHash = false, foundCoverage = false;
			
			for(int i=0; i < num_channels; i++)
			{
				PF_Boolean found;
				PF_ChannelRef channelRef;
				PF_ChannelDesc channelDesc;
				
				cs->PF_GetLayerChannelIndexedRefAndDesc(in_data->effect_ref,
														CRYPTO_INPUT,
														i,
														&found,
														&channelRef,
														&channelDesc);
				
				if(found && channelDesc.channel_type && channelDesc.data_type == PF_DataType_FLOAT && channelDesc.dimension == 1)
				{
					if(channelDesc.name == nextHashName || channelDesc.name == nextCoverageName)
					{
						if(channelDesc.name == nextHashName)
						{
							hash = channelRef;
							foundHash = true;
						}
						else
						{
							coverage = channelRef;
							foundCoverage = true;
						}
						
						if(foundHash && foundCoverage)
						{
							numLevels += 1;
							
							channelRefs.push_back(hash);
							channelRefs.push_back(coverage);
							
							CalculateNextNames(nextHashName, nextCoverageName, (NamingStyle)s, numLevels);
							foundHash = false;
							foundCoverage = false;
							
							i = 0; // start over
						}
					}
				}
			}
		}
		
		if(numLevels > 0)
		{
			assert(channelRefs.size() > 0);
			
			_buffer = new CryptomatteBuffer(in_data, channelRefs, numLevels);
		}
	}
	
	_downsampleX = in_data->downsample_x;
	_downsampleY = in_data->downsample_y;
	_currentTime = in_data->current_time;
}


void
CryptomatteContext::GetCoverage(PF_PixelFloat *row, unsigned int len, int x, int y) const
{
	PF_PixelFloat *pix = row;

	const CryptomatteBuffer::Level *level = _buffer->GetLevelGroup(x, y);
	
	const unsigned int numLevels = _buffer->NumLevels();
	
	while(len--)
	{
		float coverage = 0.f;
	
		for(int i=0; i < numLevels; i++)
		{
			if(level->coverage == 0.f)
			{
				level += (numLevels - i);
				break;
			}
				
			if( _float_selection.count(level->hash) )
			{
				coverage += level->coverage;
			}
			
			level++;
		}
		
		pix->alpha = coverage;
		
		pix++;
	}
}


void
CryptomatteContext::GetColor(PF_PixelFloat *row, unsigned int len, int x, int y, bool matted) const
{
	PF_PixelFloat *pix = row;
	
	const CryptomatteBuffer::Level *level = _buffer->GetLevelGroup(x, y);
	
	const unsigned int numLevels = _buffer->NumLevels();
	
	while(len--)
	{
		pix->alpha = pix->red = pix->green = pix->blue = 0.f;
		
		float coverage = 0.f;
	
		for(int i=0; i < numLevels; i++)
		{
			if(level->coverage == 0.f)
			{
				level += (numLevels - i);
				break;
			}
		
			if( _float_selection.count(level->hash) )
			{
				coverage += level->coverage;
			}
			
			int exp;
			
			// this method copied from the Nuke plug-in
			pix->red	+= level->coverage * fmodf(frexpf(fabsf(level->hash), &exp) * 1, 0.25);
			pix->green	+= level->coverage * fmodf(frexpf(fabsf(level->hash), &exp) * 4, 0.25);
			pix->blue	+= level->coverage * fmodf(frexpf(fabsf(level->hash), &exp) * 16, 0.25);
			
			level++;
		}
		
		if(coverage > 0.f)
		{
			pix->red	+= (coverage * (1.0f - pix->red));
			pix->green	+= (coverage * (1.0f - pix->green));
			pix->blue	+= (coverage * (1.0f - pix->blue));
		}
		
		pix->alpha = (matted ? coverage : 1.f);
		
		pix++;
	}
}


void
CryptomatteContext::GetSelectionColor(PF_PixelFloat *row, unsigned int len, int x, int y) const
{
	PF_PixelFloat *pix = row;
	
	const unsigned int numLevels = _buffer->NumLevels();
	
	const CryptomatteBuffer::Level *level = _buffer->GetLevelGroup(x, y);

	while(len--)
	{
		pix->alpha = 1.f;
		
		if(numLevels >= 1)
		{
			pix->red = level->hash;
		}
		else
			pix->red = 0.f;
		
		pix->green = 0.f; // used to put coverage here when this mode was visible

		if(numLevels >= 2)
		{
			level++;
			
			pix->blue = level->hash;
			
			level += (numLevels - 1);
		}
		else
		{
			pix->blue = 0.f;
			
			level += numLevels;
		}
		
		pix++;
	}
}


std::set<std::string>
CryptomatteContext::GetItems(int x, int y) const
{
	std::set<std::string> items;
	
	const CryptomatteBuffer::Level *level = _buffer->GetLevelGroup(x, y);
	
	for(int i=0; i < _buffer->NumLevels(); i++)
	{
		if(level->coverage > 0.f)
		{
			const Hash hash = FloatHashToHash(level->hash);
			
			if(hash > 0)
				items.insert( ItemForHash(hash) );
		}
		else
			break;
		
		level++;
	}
	
	return items;
}


std::set<std::string>
CryptomatteContext::GetItemsFromSelectionColor(const PF_PixelFloat &pixel) const
{
	std::set<std::string> items;

	const Hash red = FloatHashToHash(pixel.red);

	if(red != 0)
	{
		items.insert( ItemForHash(red) );
	}

	const Hash blue = FloatHashToHash(pixel.blue);

	if(blue != 0)
	{
		items.insert( ItemForHash(blue) );
	}

	return items;
}


std::string
CryptomatteContext::enQuote(const std::string &s)
{
	return std::string("\"") + searchReplace(s, "\"", "\\\"") + std::string("\"");
}


std::string
CryptomatteContext::enQuoteIfNecessary(const std::string &s, const std::string &quoteChars)
{
	const bool isNecessary = (std::string::npos != s.find_first_of(quoteChars));
	
	if(isNecessary)
	{
		return enQuote(s);
	}
	else
		return s;
}


std::string
CryptomatteContext::searchReplace(const std::string &str, const std::string &search, const std::string &replace)
{
	std::string s = str;
	
	// locate the search strings
	std::vector<std::string::size_type> positions;

	std::string::size_type last_pos = 0;

	while(last_pos != std::string::npos && last_pos < s.size())
	{
		last_pos = s.find(search, last_pos);

		if(last_pos != std::string::npos)
		{
			positions.push_back(last_pos);
		
			last_pos += search.size();
		}
	}

	// replace with the replace string, starting from the end
	int i = positions.size();

	while(i--)
	{
		s.erase(positions[i], search.size());
		s.insert(positions[i], replace);
	}
	
	return s;
}


std::string
CryptomatteContext::deQuote(const std::string &s)
{
	std::string::size_type start_pos = (s[0] == '\"' ? 1 : 0);
	std::string::size_type end_pos = ( (s.size() >= 2 && s[s.size()-1] == '\"' && s[s.size()-2] != '\\') ? s.size()-2 : s.size()-1);

	return searchReplace(s.substr(start_pos, end_pos + 1 - start_pos), "\\\"", "\"");
}


void
CryptomatteContext::quotedTokenize(const std::string &str, std::vector<std::string> &tokens, const std::string& delimiters)
{
	// this function will respect quoted strings when tokenizing
	// the quotes will be included in the returned strings
	
	int i = 0;
	bool in_quotes = false;
	
	// if there are un-quoted delimiters in the beginning, skip them
	while(i < str.size() && str[i] != '\"' && std::string::npos != delimiters.find(str[i]) )
		i++;
	
	std::string::size_type lastPos = i;
	
	while(i < str.size())
	{
		if(str[i] == '\"' && (i == 0 || str[i-1] != '\\'))
			in_quotes = !in_quotes;
		else if(!in_quotes)
		{
			if( std::string::npos != delimiters.find(str[i]) )
			{
				tokens.push_back(str.substr(lastPos, i - lastPos));
				
				lastPos = i + 1;
				
				// if there are more delimiters ahead, push forward
				while(lastPos < str.size() && (str[lastPos] != '\"' || str[lastPos-1] != '\\') && std::string::npos != delimiters.find(str[lastPos]) )
					lastPos++;
					
				i = lastPos;
				continue;
			}
		}
		
		i++;
	}
	
	if(in_quotes)
		throw CryptomatteException("Quoted tokenize error.");
	
	// we're at the end, was there anything left?
	if(str.size() - lastPos > 0)
		tokens.push_back( str.substr(lastPos) );
}


unsigned int
CryptomatteContext::Width() const
{
	return (_buffer != NULL ? _buffer->Width() : 0);
}


unsigned int
CryptomatteContext::Height() const
{
	return (_buffer != NULL ? _buffer->Height() : 0);
}


CryptomatteContext::FloatHash
CryptomatteContext::HashToFloatHash(const Hash &hash)
{
	FloatHash result;
	
	memcpy(&result, &hash, 4);
	
	return result;
}


Hash
CryptomatteContext::FloatHashToHash(const FloatHash &floatHash)
{
	Hash result;
	
	memcpy(&result, &floatHash, 4);
	
	return result;
}


Hash
CryptomatteContext::HashName(const std::string &name)
{
	Hash hash;
	MurmurHash3_x86_32(name.c_str(), name.length(), 0, &hash);

	// if all exponent bits are 0 (subnormals, +zero, -zero) set exponent to 1
	// if all exponent bits are 1 (NaNs, +inf, -inf) set exponent to 254
	Hash exponent = hash >> 23 & 255; // extract exponent (8 bits)
	if(exponent == 0 || exponent == 255)
		hash ^= 1 << 23; // toggle bit

	return hash;
}


bool
CryptomatteContext::GetHashIfLiteral(const std::string &name, Hash &result)
{
	// returns true if a literal value, and writes the hash to result. 
	if(name.size() == 10)
	{
		unsigned int intValue = 0;
		
		const int matched = sscanf(name.c_str(), "<%x>", &intValue);
		
		if(matched)
		{
			result = intValue;
			return true;
		}
	}
	
	return false;
}


std::string
CryptomatteContext::HashToLiteralStr(Hash hash)
{
	char hexStr[11];
	
	sprintf(hexStr, "<%08x>", hash);
	
	return std::string(hexStr);
}


typedef struct CryptomatteBufferIterateData {
	char *buf;
	unsigned int dimension;
	unsigned int numLevels;
	char *origin;
	unsigned int width;
	ptrdiff_t xStride;
	ptrdiff_t yStride;
	
	CryptomatteBufferIterateData(char *b, unsigned int d, unsigned int n, char *o, unsigned int w, ptrdiff_t x, ptrdiff_t y) :
		buf(b),
		dimension(d),
		numLevels(n),
		origin(o),
		width(w),
		xStride(x),
		yStride(y)
		{}
} FloatBufferIterateData;


static PF_Err
CryptomatteBuffer_Iterate(void *refconPV,
					A_long thread_indexL,
					A_long i,
					A_long iterationsL)
{
	CryptomatteBufferIterateData *i_data = (CryptomatteBufferIterateData *)refconPV;
	
	const size_t rowbytes = sizeof(float) * 2 * i_data->numLevels * i_data->width;
	
	float *in = (float *)(i_data->origin + (i * i_data->yStride));
	float *out = (float *)(i_data->buf + (i * rowbytes));
	
	if(i_data->dimension == 4)
	{
		const int inStep = (i_data->xStride / sizeof(float));
		const int outStep = (i_data->numLevels * 2);
		
		float *a = (in + 0);
		float *r = (in + 1);
		float *g = (in + 2);
		float *b = (in + 3);
		
		float *h1 = (out + 0);
		float *c1 = (out + 1);
		float *h2 = (out + 2);
		float *c2 = (out + 3);
		
		for(int x=0; x < i_data->width; x++)
		{
			*h1 = *r;
			*c1 = *g;
			*h2 = *b;
			*c2 = *a;
			
			r += inStep;
			g += inStep;
			b += inStep;
			a += inStep;
			
			h1 += outStep;
			c1 += outStep;
			h2 += outStep;
			c2 += outStep;
		}
	}
	else
	{
		assert(i_data->dimension == 1);
		
		const int inStep = (i_data->xStride / sizeof(float));
		const int outStep = (i_data->numLevels * 2);
	
		for(int x=0; x < i_data->width; x++)
		{
			*out = *in;
			
			in += inStep;
			out += outStep;
		}
	}

	return PF_Err_NONE;
}


CryptomatteContext::CryptomatteBuffer::CryptomatteBuffer(PF_InData *in_data, std::vector<PF_ChannelRef> &channelRefs, unsigned int numLevels) :
	_buf(NULL),
	_width(0),
	_height(0),
	_numLevels(numLevels)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	PF_ChannelSuite *cs = suites.PFChannelSuite();

	unsigned int levelNum = 0;
	
	for(int c=0; c < channelRefs.size(); c++)
	{
		PF_ChannelRef &channelRef = channelRefs[c];
	
		PF_ChannelChunk channelChunk;
		
		PF_Err err = cs->PF_CheckoutLayerChannel(in_data->effect_ref,
													&channelRef,
													in_data->current_time,
													in_data->time_step,
													in_data->time_scale,
													PF_DataType_FLOAT,
													&channelChunk);
													
		if(err == PF_Err_NONE && channelChunk.dataPV != NULL)
		{
			assert(channelChunk.data_type == PF_DataType_FLOAT);
			
			if(_buf == NULL)
			{
				assert(_width == 0 && _height == 0);
				
				_width = channelChunk.widthL;
				_height = channelChunk.heightL;
			
				const size_t siz = (_width * _height * _numLevels * sizeof(Level));
				
				_buf = (char *)malloc(siz);
				
				if(_buf == NULL)
					throw CryptomatteException("Memory error");
			}
			else
			{
				assert(channelChunk.widthL == _width);
				assert(channelChunk.heightL == _height);
			}
		
			if(channelChunk.dimensionL == 4)
			{
				CryptomatteBufferIterateData iter(_buf + (levelNum * sizeof(Level)), 4, _numLevels, (char *)channelChunk.dataPV, _width, sizeof(float) * 4, channelChunk.row_bytesL);

				suites.PFIterate8Suite()->iterate_generic(_height, &iter, CryptomatteBuffer_Iterate);
				
				levelNum += 2;
			}
			else
			{
				assert(channelChunk.dimensionL == 1);
				
				CryptomatteBufferIterateData hash_iter(_buf + (levelNum * sizeof(Level)), 1, _numLevels, (char *)channelChunk.dataPV, _width, sizeof(float), channelChunk.row_bytesL);

				suites.PFIterate8Suite()->iterate_generic(_height, &hash_iter, CryptomatteBuffer_Iterate);
				
				
				assert(channelRefs.size() > (c + 1));
				
				PF_ChannelRef &coverageChannelRef = channelRefs[c + 1];
				
				PF_ChannelChunk coverageChannelChunk;
				
				err = cs->PF_CheckoutLayerChannel(in_data->effect_ref,
													&coverageChannelRef,
													in_data->current_time,
													in_data->time_step,
													in_data->time_scale,
													PF_DataType_FLOAT,
													&coverageChannelChunk);
													
				if(err == PF_Err_NONE && coverageChannelChunk.dataPV != NULL)
				{
					assert(coverageChannelChunk.data_type == PF_DataType_FLOAT);
					assert(coverageChannelChunk.widthL == _width);
					assert(coverageChannelChunk.heightL == _height);
					
					CryptomatteBufferIterateData coverage_iter(_buf + (levelNum * sizeof(Level)) + sizeof(float), 1, _numLevels, (char *)channelChunk.dataPV, _width, sizeof(float), channelChunk.row_bytesL);

					suites.PFIterate8Suite()->iterate_generic(_height, &coverage_iter, CryptomatteBuffer_Iterate);
					
					cs->PF_CheckinLayerChannel(in_data->effect_ref, &coverageChannelRef, &coverageChannelChunk);
				}
				else
					assert(FALSE);
				
				c++;
				
				levelNum += 1;
			}
			
			cs->PF_CheckinLayerChannel(in_data->effect_ref, &channelRef, &channelChunk);
		}
		else
			assert(FALSE);
	}
}


CryptomatteContext::CryptomatteBuffer::~CryptomatteBuffer()
{
	if(_buf)
		free(_buf);
}


std::string
CryptomatteContext::ItemForHash(const Hash &hash) const
{
	// first check the selection
	if(_selection.length())
	{
		std::vector<std::string> tokens;
		quotedTokenize(_selection, tokens, ", ");

		for(std::vector<std::string>::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
		{
			const std::string val = deQuote(*i);

			Hash literalHash;
			if(GetHashIfLiteral(val, literalHash) && literalHash == hash)
				return val;

			if(HashName(val) == hash)
				return val;
		}
	}

	// then check the manifest
	for(std::map<std::string, Hash>::const_iterator j = _manifest.begin(); j != _manifest.end(); ++j)
	{
		const std::string &name = j->first;
		const Hash &value = j->second;

		if(hash == value)
			return name;
	}

	// finally, use a hex code
	return HashToLiteralStr(hash);
}


void
CryptomatteContext::CalculateNextNames(std::string &nextHashName, std::string &nextCoverageName, NamingStyle style, int levels) const
{
	const int layerNum = (levels / 2);
	const bool useBA = (levels % 2);
	
	std::stringstream ss1, ss2;
	
	ss1 << _layer << std::setw(2) << std::setfill('0') << layerNum << ".";
	ss2 << _layer << std::setw(2) << std::setfill('0') << layerNum << ".";
	
	if(style == NAMING_rgba)
	{
		if(useBA)
		{
			ss1 << "b";
			ss2 << "a";
		}
		else
		{
			ss1 << "r";
			ss2 << "g";
		}
	}
	else if(style == NAMING_redgreenbluealpha)
	{
		if(useBA)
		{
			ss1 << "blue";
			ss2 << "alpha";
		}
		else
		{
			ss1 << "red";
			ss2 << "green";
		}
	}
	else
	{
		if(useBA)
		{
			ss1 << "B";
			ss2 << "A";
		}
		else
		{
			ss1 << "R";
			ss2 << "G";
		}
	}
	
	nextHashName = ss1.str();
	nextCoverageName = ss2.str();
}


void
CryptomatteContext::CalculateNext4Name(std::string &fourName, NamingStyle style, int levels) const
{
	const int layerNum = (levels / 2);
	
	std::stringstream ss;
	
	ss << _layer << std::setw(2) << std::setfill('0') << layerNum;
	
	if(style == NAMING_rgba)
	{
		ss << ".argb";
	}
	else if(style == NAMING_redgreenbluealpha)
	{
		ss << ".alpharedgreenblue";
	}
	else
	{
		ss << ".ARGB";
	}
	
	fourName = ss.str();
}

#pragma mark-


static PF_Err 
About (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_SPRINTF(	out_data->return_msg, 
				"%s - %s\r\rwritten by %s\r\rv%d.%d - %s\r\r%s\r%s",
				NAME,
				DESCRIPTION,
				AUTHOR, 
				MAJOR_VERSION, 
				MINOR_VERSION,
				RELEASE_DATE,
				COPYRIGHT,
				WEBSITE);
				
	return PF_Err_NONE;
}


AEGP_PluginID gAEGPPluginID;

static PF_Err 
GlobalSetup (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	out_data->my_version 	= 	PF_VERSION(	MAJOR_VERSION, 
											MINOR_VERSION,
											BUG_VERSION, 
											STAGE_VERSION, 
											BUILD_VERSION);

	out_data->out_flags 	= 	PF_OutFlag_DEEP_COLOR_AWARE		|
								PF_OutFlag_PIX_INDEPENDENT		|
								PF_OutFlag_CUSTOM_UI			|
							#ifdef WIN_ENV
								PF_OutFlag_KEEP_RESOURCE_OPEN	|
							#endif
								PF_OutFlag_USE_OUTPUT_EXTENT;

	out_data->out_flags2 	=	PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG |
								PF_OutFlag2_SUPPORTS_SMART_RENDER	|
							#if AE135_RENDER_THREAD_MADNESS
								PF_OutFlag2_SUPPORTS_GET_FLATTENED_SEQUENCE_DATA |
							#endif
								PF_OutFlag2_FLOAT_COLOR_AWARE;


#if AE135_RENDER_THREAD_MADNESS
	if(in_data->version.major == PF_AE135_PLUG_IN_VERSION && in_data->version.minor < PF_AE135_PLUG_IN_SUBVERS)
	{
		PF_SPRINTF(out_data->return_msg, "Your version of the Cryptomatte plug-in is meant for After Effects CC 2015 and later. "
											"Please use the CS6 version.");

		return PF_Err_BAD_CALLBACK_PARAM;
	}
#else
	if(in_data->version.major == PF_AE135_PLUG_IN_VERSION && in_data->version.minor >= PF_AE135_PLUG_IN_SUBVERS)
	{
		PF_SPRINTF(out_data->return_msg, "Your version of the Cryptomatte plug-in is meant for After Effects CC 2014 and earlier. "
											"Please the regular version, not the CS6 version.");

		return PF_Err_BAD_CALLBACK_PARAM;
	}
#endif


	AEGP_SuiteHandler suites(in_data->pica_basicP);
	suites.UtilitySuite()->AEGP_RegisterWithAEGP(NULL, NAME, &gAEGPPluginID);

	return PF_Err_NONE;
}


static PF_Err 
GlobalSetdown (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
#ifndef NDEBUG
	assert(gNumContexts == 0);
#endif

	return PF_Err_NONE;
}


static PF_Err
ParamsSetup (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	PF_Err 			err = PF_Err_NONE;
	PF_ParamDef		def;


	// readout
	AEFX_CLR_STRUCT(def);
#define ARB_REFCON NULL // used by PF_ADD_ARBITRARY
	
	ArbNewDefault(in_data, out_data, ARB_REFCON, &def.u.arb_d.dephault);

	PF_ADD_ARBITRARY2("Settings",
						kUI_CONTROL_WIDTH,
						kUI_CONTROL_HEIGHT,
						0, //PF_ParamFlag_CANNOT_TIME_VARY, // is it a problem that I can't cut and paste keyframes?
						PF_PUI_CONTROL,
						def.u.arb_d.dephault,
						ARBITRARY_DATA_ID,
						ARB_REFCON);
	

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Output",
					DISPLAY_NUM_OPTIONS, //number of choices
					DISPLAY_COLORS, //default
					DISPLAY_MENU_STR,
					DISPLAY_ID);
	

	AEFX_CLR_STRUCT(def);
	def.ui_flags = PF_PUI_INVISIBLE;
	PF_ADD_CHECKBOX("Selection Mode", "fnord!", // nobody will see this anyway
						FALSE,
						PF_ParamFlag_CANNOT_TIME_VARY,
						SELECTION_MODE_ID);


	out_data->num_params = CRYPTO_NUM_PARAMS;

	// register custom UI
	if(!err) 
	{
		PF_CustomUIInfo			ci;

		AEFX_CLR_STRUCT(ci);
		
		ci.events				= PF_CustomEFlag_EFFECT | PF_CustomEFlag_LAYER | PF_CustomEFlag_COMP;
 		
		ci.comp_ui_width		= ci.comp_ui_height = 0;
		ci.comp_ui_alignment	= PF_UIAlignment_NONE;
		
		ci.layer_ui_width		= 0;
		ci.layer_ui_height		= 0;
		ci.layer_ui_alignment	= PF_UIAlignment_NONE;
		
		ci.preview_ui_width		= 0;
		ci.preview_ui_height	= 0;
		ci.layer_ui_alignment	= PF_UIAlignment_NONE;

		err = (*(in_data->inter.register_ui))(in_data->effect_ref, &ci);
	}


	return err;
}


static PF_Err
SequenceSetup (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err err = PF_Err_NONE;
	
	CryptomatteSequenceData *sequence_data = NULL;
	
	// set up sequence data
	if(in_data->sequence_data == NULL)
	{
		out_data->sequence_data = PF_NEW_HANDLE( sizeof(CryptomatteSequenceData) );
		
		sequence_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(out_data->sequence_data);
		
		// set defaults
		sequence_data->context = NULL;
		sequence_data->selectionChanged = FALSE;
		
		PF_UNLOCK_HANDLE(out_data->sequence_data);
	}
	else // reset pre-existing sequence data
	{
		if(PF_GET_HANDLE_SIZE(in_data->sequence_data) != sizeof(CryptomatteSequenceData))
		{
			PF_RESIZE_HANDLE(sizeof(CryptomatteSequenceData), &in_data->sequence_data);
		}
			
		sequence_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(in_data->sequence_data);
		
		// set defaults
		sequence_data->context = NULL;
		sequence_data->selectionChanged = FALSE;
		
		PF_UNLOCK_HANDLE(in_data->sequence_data);
	}
	
	return err;
}


static PF_Err 
SequenceSetdown (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err err = PF_Err_NONE;
	
	if(in_data->sequence_data)
	{
		CryptomatteSequenceData *seq_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(in_data->sequence_data);
		
		if(seq_data->context != NULL)
		{
			CryptomatteContext *ctx = (CryptomatteContext *)seq_data->context;
			
			delete ctx;
		}
	
		PF_DISPOSE_HANDLE(in_data->sequence_data);
	}

	return err;
}


static PF_Err 
SequenceFlatten (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	if(in_data->sequence_data)
	{
		CryptomatteSequenceData *in_sequence_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(in_data->sequence_data);
		
		CryptomatteContext *ctx = (CryptomatteContext *)in_sequence_data->context;
		
		delete ctx;
		
		in_sequence_data->context = NULL;
		
		PF_UNLOCK_HANDLE(in_data->sequence_data);
	}

	return PF_Err_NONE;
}


static PF_Err 
GetFlattenedSequenceData(	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	if(in_data->sequence_data)
	{
		CryptomatteSequenceData *in_sequence_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(in_data->sequence_data);

		out_data->sequence_data = PF_NEW_HANDLE(sizeof(CryptomatteSequenceData));

		CryptomatteSequenceData *out_sequence_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(out_data->sequence_data);
		
		assert(in_sequence_data->selectionChanged == FALSE); // not using selectionChanged in the version that uses this call

		out_sequence_data->selectionChanged = in_sequence_data->selectionChanged;

		out_sequence_data->context = NULL;

		PF_UNLOCK_HANDLE(in_data->sequence_data);
		PF_UNLOCK_HANDLE(out_data->sequence_data);
	}

	return PF_Err_NONE;
}


static PF_Boolean
IsEmptyRect(const PF_LRect *r){
	return (r->left >= r->right) || (r->top >= r->bottom);
}

#ifndef mmin
	#define mmin(a,b) ((a) < (b) ? (a) : (b))
	#define mmax(a,b) ((a) > (b) ? (a) : (b))
#endif


static void
UnionLRect(const PF_LRect *src, PF_LRect *dst)
{
	if (IsEmptyRect(dst)) {
		*dst = *src;
	} else if (!IsEmptyRect(src)) {
		dst->left 	= mmin(dst->left, src->left);
		dst->top  	= mmin(dst->top, src->top);
		dst->right 	= mmax(dst->right, src->right);
		dst->bottom = mmax(dst->bottom, src->bottom);
	}
}


static PF_Err
PreRender(
	PF_InData				*in_data,
	PF_OutData				*out_data,
	PF_PreRenderExtra		*extra)
{
	PF_Err err = PF_Err_NONE;
	PF_RenderRequest req = extra->input->output_request;
	PF_CheckoutResult in_result;
	
	req.preserve_rgb_of_zero_alpha = TRUE;

	ERR(extra->cb->checkout_layer(	in_data->effect_ref,
									CRYPTO_INPUT,
									CRYPTO_INPUT,
									&req,
									in_data->current_time,
									in_data->time_step,
									in_data->time_scale,
									&in_result));


	UnionLRect(&in_result.result_rect, 		&extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, 	&extra->output->max_result_rect);	
	
	// BTW, just because we checked out the layer here, doesn't mean we really
	// have to check it out.  
	
	return err;
}

#pragma mark-

static inline float Clamp(const float &val)
{
	return (val > 1.f ? 1.f : (val < 0.f ? 0.f : val));
}

template <typename T>
static inline T FloatToChan(const float &val);

template <>
static inline PF_FpShort FloatToChan<PF_FpShort>(const float &val)
{
	return val;
}

template <>
static inline A_u_short FloatToChan<A_u_short>(const float &val)
{
	return ((Clamp(val) * (float)PF_MAX_CHAN16) + 0.5f);
}

template <>
static inline A_u_char FloatToChan<A_u_char>(const float &val)
{
	return ((Clamp(val) * (float)PF_MAX_CHAN8) + 0.5f);
}


typedef struct MatteIterateData {
	PF_InData			*in_data;
	CryptomatteContext	*context;
	PF_PixelPtr			data;
	A_long				rowbytes;
	PF_Point			channelMove;
	A_long				width;
	int					display;
	bool				selection;
	
	MatteIterateData(PF_InData *in, CryptomatteContext *c, PF_PixelPtr d, A_long rb, PF_Point ch, A_long w, int di, bool s) :
		in_data(in),
		context(c),
		data(d),
		rowbytes(rb),
		channelMove(ch),
		width(w),
		display(di),
		selection(s) {}
} MatteIterateData;


template <typename PIXTYPE, typename CHANTYPE>
static PF_Err
DrawMatte_Iterate(void *refconPV,
					A_long thread_indexL,
					A_long i,
					A_long iterationsL)
{
	PF_Err err = PF_Err_NONE;
	
	MatteIterateData *i_data = (MatteIterateData *)refconPV;
	PF_InData *in_data = i_data->in_data;
	
	PIXTYPE *pix = (PIXTYPE *)((char *)i_data->data + ((i + i_data->channelMove.v) * i_data->rowbytes) + (i_data->channelMove.h * sizeof(PIXTYPE)));
	
	if(i_data->selection)
	{
		if(sizeof(PIXTYPE) == sizeof(PF_PixelFloat))
		{
			i_data->context->GetSelectionColor((PF_PixelFloat *)pix, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v);
		}
		else
		{
			PF_PixelFloat *tempRow = (PF_PixelFloat *)malloc(i_data->width * sizeof(PF_PixelFloat));
			
			if(tempRow != NULL)
			{
				i_data->context->GetSelectionColor(tempRow, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v);
				
				PF_PixelFloat *tempPix = tempRow;
				
				for(int x=0; x < i_data->width; x++)
				{
					pix->alpha	= FloatToChan<CHANTYPE>(tempPix->alpha);
					pix->red	= FloatToChan<CHANTYPE>(tempPix->red);
					pix->green	= FloatToChan<CHANTYPE>(tempPix->green);
					pix->blue	= FloatToChan<CHANTYPE>(tempPix->blue);
					
					pix++;
					tempPix++;
				}
				
				free(tempRow);
			}
		}
	}
	else if(i_data->display == DISPLAY_COLORS || i_data->display == DISPLAY_MATTED_COLORS)
	{
		const bool matted = (i_data->display == DISPLAY_MATTED_COLORS);
		
		if(sizeof(PIXTYPE) == sizeof(PF_PixelFloat))
		{
			i_data->context->GetColor((PF_PixelFloat *)pix, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v, matted);
		}
		else
		{
			PF_PixelFloat *tempRow = (PF_PixelFloat *)malloc(i_data->width * sizeof(PF_PixelFloat));
			
			if(tempRow != NULL)
			{
				i_data->context->GetColor(tempRow, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v, matted);
				
				PF_PixelFloat *tempPix = tempRow;
				
				for(int x=0; x < i_data->width; x++)
				{
					pix->alpha	= FloatToChan<CHANTYPE>(tempPix->alpha);
					pix->red	= FloatToChan<CHANTYPE>(tempPix->red);
					pix->green	= FloatToChan<CHANTYPE>(tempPix->green);
					pix->blue	= FloatToChan<CHANTYPE>(tempPix->blue);
					
					pix++;
					tempPix++;
				}
				
				free(tempRow);
			}
		}
	}
	else
	{
		if(sizeof(PIXTYPE) == sizeof(PF_PixelFloat))
		{
			i_data->context->GetCoverage((PF_PixelFloat *)pix, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v);
		}
		else
		{
			PF_PixelFloat *tempRow = (PF_PixelFloat *)malloc(i_data->width * sizeof(PF_PixelFloat));
			
			if(tempRow != NULL)
			{
				i_data->context->GetCoverage(tempRow, i_data->width, i_data->channelMove.h, i + i_data->channelMove.v);
				
				PF_PixelFloat *tempPix = tempRow;
				
				for(int x=0; x < i_data->width; x++)
				{
					pix->alpha = FloatToChan<CHANTYPE>(tempPix->alpha);
					
					pix++;
					tempPix++;
				}
				
				free(tempRow);
			}
		}
	}

#ifdef NDEBUG
	if(thread_indexL == 0)
		err = PF_ABORT(in_data);
#endif

	return err;
}


typedef struct MergeIterateData {
	PF_InData		*in_data;
	PF_EffectWorld	*alpha;
	PF_EffectWorld	*input;
	PF_EffectWorld	*output;
	PF_Point		worldMove;
	PF_Point		channelMove;
	A_long			width;
	int				display;
	
	MergeIterateData(PF_InData *in, PF_EffectWorld *a, PF_EffectWorld *i, PF_EffectWorld *o, PF_Point w, PF_Point c, A_long wd, int d) :
		in_data(in),
		alpha(a),
		input(i),
		output(o),
		worldMove(w),
		channelMove(c),
		width(wd),
		display(d) {}
} MergeIterateData;


template <typename PIXTYPE, typename CHANTYPE>
static PF_Err
Merge_Iterate(void *refconPV,
				A_long thread_indexL,
				A_long i,
				A_long iterationsL)
{
	PF_Err err = PF_Err_NONE;
	
	MergeIterateData *i_data = (MergeIterateData *)refconPV;
	PF_InData *in_data = i_data->in_data;
	
	PIXTYPE *alpha = (PIXTYPE *)((char *)i_data->alpha->data + ((i + i_data->channelMove.v) * i_data->alpha->rowbytes) + (i_data->channelMove.h * sizeof(PIXTYPE)));
	PIXTYPE *output = (PIXTYPE *)((char *)i_data->output->data + ((i + i_data->worldMove.v) * i_data->output->rowbytes) + (i_data->worldMove.h * sizeof(PIXTYPE)));
	
	if(i_data->display == DISPLAY_MATTE_ONLY)
	{
		const CHANTYPE white = FloatToChan<CHANTYPE>(1.f);
	
		for(int x=0; x < i_data->width; x++)
		{
			output->alpha = white;
			output->blue = output->green = output->red = alpha->alpha;
			
			alpha++;
			output++;
		}
	}
	else
	{
		PIXTYPE *input = (PIXTYPE *)((char *)i_data->input->data + ((i + i_data->worldMove.v) * i_data->input->rowbytes) + (i_data->worldMove.h * sizeof(PIXTYPE)));

		for(int x=0; x < i_data->width; x++)
		{
			output->alpha = alpha->alpha;
			output->red = input->red;
			output->green = input->green;
			output->blue = input->blue;
		
			alpha++;
			input++;
			output++;
		}
	}

#ifdef NDEBUG
	if(thread_indexL == 0)
		err = PF_ABORT(in_data);
#endif

	return err;
}


#pragma mark-

static PF_Err
DoRender(
		PF_InData		*in_data,
		PF_EffectWorld 	*input,
		PF_ParamDef		*CRYPTO_data,
		PF_ParamDef		*CRYPTO_display,
		PF_ParamDef		*CRYPTO_selection,
		PF_OutData		*out_data,
		PF_EffectWorld	*output)
{
	PF_Err ae_err = PF_Err_NONE;
	
	CryptomatteArbitraryData *arb_data = (CryptomatteArbitraryData *)PF_LOCK_HANDLE(CRYPTO_data->u.arb_d.value);
	CryptomatteSequenceData *seq_data = (CryptomatteSequenceData *)PF_LOCK_HANDLE(in_data->sequence_data);

	PF_EffectWorld alphaWorldData;
	PF_EffectWorld *alphaWorld = NULL;
	
	AEGP_SuiteHandler suites(in_data->pica_basicP);

	try
	{
		ErrThrower err;

		CryptomatteContext *context = (CryptomatteContext *)seq_data->context;
		
		if(context == NULL)
		{
			seq_data->context = context = new CryptomatteContext(arb_data);
		
			context->LoadLevels(in_data);
		}
		else
		{
			context->Update(arb_data);

			// did the selection just change, so we don't have to reload the Cryptomatte levels?
		#if AE135_RENDER_THREAD_MADNESS
			const bool selectionJustChanged = CRYPTO_selection->u.bd.value;
		#else
			const bool selectionJustChanged = seq_data->selectionChanged;
		#endif

			if(!selectionJustChanged ||
				context->CurrentTime() != in_data->current_time ||
				context->DownsampleX().num != in_data->downsample_x.num ||
				context->DownsampleX().den != in_data->downsample_x.den ||
				context->DownsampleY().num != in_data->downsample_y.num ||
				context->DownsampleY().den != in_data->downsample_y.den)
			{
				// don't re-load levels if the selection JUST changed
				// hopefully people don't switch frames in between the click and the render,
				// say if caps lock was down
				
				context->LoadLevels(in_data);
			}
		}
		
		seq_data->selectionChanged = FALSE;
		
	
		if( context->Valid() )
		{
			PF_PixelFormat format = PF_PixelFormat_INVALID;
			suites.PFWorldSuite()->PF_GetPixelFormat(output, &format);
			
			// make pixel world for Cryptomatte, black RGB with alpha
			alphaWorld = &alphaWorldData;
			suites.PFWorldSuite()->PF_NewWorld(in_data->effect_ref, context->Width(), context->Height(), TRUE, format, alphaWorld);
			

			// the origin might not be 0,0 and the ROI might not include the whole image
			// we have to figure out where we have to move our pointers to the right spot in each buffer
			// and copy only as far as we can

			// if the origin is negative, we move in the world, if positive, we move in the channel
			PF_Point world_move, chan_move;
			
			world_move.h = MAX(-output->origin_x, 0);
			world_move.v = MAX(-output->origin_y, 0);
			
			chan_move.h = MAX(output->origin_x, 0);
			chan_move.v = MAX(output->origin_y, 0);

			const int copy_width = MIN(output->width - world_move.h, alphaWorld->width - chan_move.h);
			const int copy_height = MIN(output->height - world_move.v, alphaWorld->height - chan_move.v);


			MatteIterateData matte_iter(in_data, context, alphaWorld->data, alphaWorld->rowbytes, chan_move, copy_width, CRYPTO_display->u.pd.value, CRYPTO_selection->u.bd.value);
			
			if(format == PF_PixelFormat_ARGB128)
			{
				err = suites.PFIterate8Suite()->iterate_generic(copy_height, &matte_iter, DrawMatte_Iterate<PF_PixelFloat, PF_FpShort>);
			}
			else if(format == PF_PixelFormat_ARGB64)
			{
				err = suites.PFIterate8Suite()->iterate_generic(copy_height, &matte_iter, DrawMatte_Iterate<PF_Pixel16, A_u_short>);
			}
			else if(format == PF_PixelFormat_ARGB32)
			{
				err = suites.PFIterate8Suite()->iterate_generic(copy_height, &matte_iter, DrawMatte_Iterate<PF_Pixel, A_u_char>);
			}
			
			
			if(CRYPTO_display->u.pd.value == DISPLAY_COLORS ||
				CRYPTO_display->u.pd.value == DISPLAY_MATTED_COLORS ||
				CRYPTO_selection->u.bd.value)
			{
				if(in_data->quality == PF_Quality_HI)
					err = suites.PFWorldTransformSuite()->copy_hq(in_data->effect_ref, alphaWorld, output, NULL, NULL);
				else
					err = suites.PFWorldTransformSuite()->copy(in_data->effect_ref, alphaWorld, output, NULL, NULL);
			}
			else
			{
				MergeIterateData merge_iter(in_data, alphaWorld, input, output, world_move, chan_move, copy_width, CRYPTO_display->u.pd.value);
				
				if(format == PF_PixelFormat_ARGB128)
				{
					err = suites.PFIterate8Suite()->iterate_generic(copy_height, &merge_iter, Merge_Iterate<PF_PixelFloat, PF_FpShort>);
				}
				else if(format == PF_PixelFormat_ARGB64)
				{
					err = suites.PFIterate8Suite()->iterate_generic(copy_height, &merge_iter, Merge_Iterate<PF_Pixel16, A_u_short>);
				}
				else if(format == PF_PixelFormat_ARGB32)
				{
					err = suites.PFIterate8Suite()->iterate_generic(copy_height, &merge_iter, Merge_Iterate<PF_Pixel, A_u_char>);
				}
			}
		}
		else
		{
			if(in_data->quality == PF_Quality_HI)
				err = suites.PFWorldTransformSuite()->copy_hq(in_data->effect_ref, input, output, NULL, NULL);
			else
				err = suites.PFWorldTransformSuite()->copy(in_data->effect_ref, input, output, NULL, NULL);
		}
	}
	catch(ErrThrower &e)
	{
		ae_err = e.err();
	}
	catch(...)
	{
		ae_err = PF_Err_BAD_CALLBACK_PARAM; 
	}
	
	if(alphaWorld)
		suites.PFWorldSuite()->PF_DisposeWorld(in_data->effect_ref, alphaWorld);
	
	
	PF_UNLOCK_HANDLE(CRYPTO_data->u.arb_d.value);
	PF_UNLOCK_HANDLE(in_data->sequence_data);
	
	
	return ae_err;
}


static PF_Err
SmartRender(
	PF_InData				*in_data,
	PF_OutData				*out_data,
	PF_SmartRenderExtra		*extra)

{
	PF_Err			err		= PF_Err_NONE,
					err2 	= PF_Err_NONE;
					
	PF_EffectWorld *input, *output;
	
	PF_ParamDef CRYPTO_data,
				CRYPTO_display,
				CRYPTO_selection;

	// zero-out parameters
	AEFX_CLR_STRUCT(CRYPTO_data);
	AEFX_CLR_STRUCT(CRYPTO_display);
	AEFX_CLR_STRUCT(CRYPTO_selection);
	
	
#define PF_CHECKOUT_PARAM_NOW( PARAM, DEST )	PF_CHECKOUT_PARAM(	in_data, (PARAM), in_data->current_time, in_data->time_step, in_data->time_scale, DEST )

	// get our arb data and see if it requires the input buffer
	ERR( PF_CHECKOUT_PARAM_NOW(CRYPTO_DATA, &CRYPTO_data) );
	ERR( PF_CHECKOUT_PARAM_NOW(CRYPTO_SELECTION_MODE, &CRYPTO_selection) );
	ERR( PF_CHECKOUT_PARAM_NOW(CRYPTO_DISPLAY, &CRYPTO_display) );

	
	if(!err)
	{
		// always get the input because we don't know if something will go wrong
		err = extra->cb->checkout_layer_pixels(in_data->effect_ref, CRYPTO_INPUT, &input);
	}
	else
	{
		input = NULL;
	}
	
	
	// always get the output buffer
	ERR(	extra->cb->checkout_output(	in_data->effect_ref, &output)	);


	// checkout the required params
	ERR(DoRender(	in_data, 
					input, 
					&CRYPTO_data,
					&CRYPTO_display,
					&CRYPTO_selection,
					out_data, 
					output));

	// Always check in, no matter what the error condition!
	ERR2(	PF_CHECKIN_PARAM(in_data, &CRYPTO_data )	);
	ERR2(	PF_CHECKIN_PARAM(in_data, &CRYPTO_display )	);
	ERR2(	PF_CHECKIN_PARAM(in_data, &CRYPTO_selection )	);


	return err;
}


PF_Err 
DoDialog(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	A_Err			err 		= A_Err_NONE;
	
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	PF_ChannelSuite *cs = suites.PFChannelSuite();


	A_long chan_count = 0;
	cs->PF_GetLayerChannelCount(in_data->effect_ref, 0, &chan_count);

	if(chan_count == 0 || err)
	{
		PF_SPRINTF(out_data->return_msg, "No auxiliary channels available.");
	}
	else
	{
		CryptomatteArbitraryData *arb = (CryptomatteArbitraryData *)PF_LOCK_HANDLE(params[CRYPTO_DATA]->u.arb_d.value);
		
		#ifdef MAC_ENV
			const char *plugHndl = "com.fnordware.AfterEffects.Cryptomatte";
			const void *mwnd = NULL;
		#else
			const char *plugHndl = NULL;
			HWND *mwnd = NULL;
			PF_GET_PLATFORM_DATA(PF_PlatData_MAIN_WND, &mwnd);
		#endif
		
		std::string layer = GetLayer(arb);
		std::string selection = GetSelection(arb);
		std::string manifest = GetManifest(arb);
		
		PF_UNLOCK_HANDLE(params[CRYPTO_DATA]->u.arb_d.value);

		const bool clicked_ok = Cryptomatte_Dialog(layer, selection, manifest, plugHndl, mwnd);
		
		if(clicked_ok)
		{
			SetArb(in_data, &params[CRYPTO_DATA]->u.arb_d.value, layer, selection, manifest);
			
			params[CRYPTO_DATA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
		}
	}
	
	return err;
}


DllExport	
PF_Err 
PluginMain (	
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra)
{
	PF_Err		err = PF_Err_NONE;
	
	try	{
		switch (cmd) {
			case PF_Cmd_ABOUT:
				err = About(in_data,out_data,params,output);
				break;
			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(in_data,out_data,params,output);
				break;
			case PF_Cmd_GLOBAL_SETDOWN:
				err = GlobalSetdown(in_data,out_data,params,output);
				break;				
			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(in_data,out_data,params,output);
				break;
			case PF_Cmd_SEQUENCE_SETUP:
			case PF_Cmd_SEQUENCE_RESETUP:
				err = SequenceSetup(in_data, out_data, params, output);
				break;
			case PF_Cmd_SEQUENCE_FLATTEN:
				err = SequenceFlatten(in_data, out_data, params, output);
				break;
		#if AE135_RENDER_THREAD_MADNESS
			case PF_Cmd_GET_FLATTENED_SEQUENCE_DATA:
				err = GetFlattenedSequenceData(in_data, out_data, params, output);
				break;
		#endif
			case PF_Cmd_SEQUENCE_SETDOWN:
				err = SequenceSetdown(in_data, out_data, params, output);
				break;
			case PF_Cmd_SMART_PRE_RENDER:
				err = PreRender(in_data, out_data, (PF_PreRenderExtra *)extra);
				break;
			case PF_Cmd_SMART_RENDER:
				err = SmartRender(in_data, out_data, (PF_SmartRenderExtra *)extra);
				break;
			case PF_Cmd_EVENT:
				err = HandleEvent(in_data, out_data, params, output, (PF_EventExtra	*)extra);
				break;
			case PF_Cmd_DO_DIALOG:
				assert(FALSE); // only we should be calling DoDialog when the users clicks our effect UI
				err = DoDialog(in_data, out_data, params, output);
				break;	
			case PF_Cmd_ARBITRARY_CALLBACK:
				err = HandleArbitrary(in_data, out_data, params, output, (PF_ArbParamsExtra	*)extra);
				break;
		}
	}
	catch(PF_Err &thrown_err) { err = thrown_err; }
	catch(...) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; }
	
	return err;
}
