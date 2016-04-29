/*
* Copyright 2016 ZXing authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "pdf417/PDFDecodedBitStreamParser.h"
#include "pdf417/PDFDecoderResultExtra.h"
#include "CharacterSetECI.h"
#include "StringCodecs.h"
#include "ZXBigInteger.h"
#include "ZXString.h"
#include "ByteArray.h"
#include "ErrorStatus.h"
#include "DecoderResult.h"

#include <array>

namespace ZXing {
namespace Pdf417 {

enum class Mode {
	ALPHA,
	LOWER,
	MIXED,
	PUNCT,
	ALPHA_SHIFT,
	PUNCT_SHIFT
};

static const int TEXT_COMPACTION_MODE_LATCH = 900;
static const int BYTE_COMPACTION_MODE_LATCH = 901;
static const int NUMERIC_COMPACTION_MODE_LATCH = 902;
static const int BYTE_COMPACTION_MODE_LATCH_6 = 924;
static const int ECI_USER_DEFINED = 925;
static const int ECI_GENERAL_PURPOSE = 926;
static const int ECI_CHARSET = 927;
static const int BEGIN_MACRO_PDF417_CONTROL_BLOCK = 928;
static const int BEGIN_MACRO_PDF417_OPTIONAL_FIELD = 923;
static const int MACRO_PDF417_TERMINATOR = 922;
static const int MODE_SHIFT_TO_BYTE_COMPACTION_MODE = 913;
static const int MAX_NUMERIC_CODEWORDS = 15;

static const int PL = 25;
static const int LL = 27;
static const int AS = 27;
static const int ML = 28;
static const int AL = 28;
static const int PS = 29;
static const int PAL = 29;

static const char PUNCT_CHARS[] = {
	';', '<', '>', '@', '[', '\\', ']', '_', '`', '~', '!',
	'\r', '\t', ',', ':', '\n', '-', '.', '$', '/', '"', '|', '*',
	'(', ')', '?', '{', '}', '\'' };

static const char MIXED_CHARS[] = {
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '&',
	'\r', '\t', ',', ':', '#', '-', '.', '$', '/', '+', '%', '*',
	'=', '^' };

static const CharacterSet DEFAULT_ENCODING = CharacterSet::ISO8859_1;

/**
* Table containing values for the exponent of 900.
* This is used in the numeric compaction decode algorithm.
*/
static const std::array<BigInteger, 16>& EXP900()
{
	auto initInstance = [](std::array<BigInteger, 16>& table)->std::array<BigInteger, 16>& {
		table[0] = BigInteger(1);
		BigInteger nineHundred(900);
		table[1] = nineHundred;
		for (size_t i = 2; i < table.size(); ++i) {
			BigInteger::Multiply(table[i - 1], nineHundred, table[i]);
		}
		return table;
	};

	static std::array<BigInteger, 16> instance;
	static const std::array<BigInteger, 16>& ref = initInstance(instance);
	return ref;
}

static const int NUMBER_OF_SEQUENCE_CODEWORDS = 2;


/**
* The Text Compaction mode includes all the printable ASCII characters
* (i.e. values from 32 to 126) and three ASCII control characters: HT or tab
* (ASCII value 9), LF or line feed (ASCII value 10), and CR or carriage
* return (ASCII value 13). The Text Compaction mode also includes various latch
* and shift characters which are used exclusively within the mode. The Text
* Compaction mode encodes up to 2 characters per codeword. The compaction rules
* for converting data into PDF417 codewords are defined in 5.4.2.2. The sub-mode
* switches are defined in 5.4.2.3.
*
* @param textCompactionData The text compaction data.
* @param byteCompactionData The byte compaction data if there
*                           was a mode shift.
* @param length             The size of the text compaction and byte compaction data.
* @param result             The decoded data is appended to the result.
*/
static void DecodeTextCompaction(const std::vector<int>& textCompactionData, const std::vector<int>& byteCompactionData, int length, String& result) {
	// Beginning from an initial state of the Alpha sub-mode
	// The default compaction mode for PDF417 in effect at the start of each symbol shall always be Text
	// Compaction mode Alpha sub-mode (uppercase alphabetic). A latch codeword from another mode to the Text
	// Compaction mode shall always switch to the Text Compaction Alpha sub-mode.
	Mode subMode = Mode::ALPHA;
	Mode priorToShiftMode = Mode::ALPHA;
	int i = 0;
	while (i < length) {
		int subModeCh = textCompactionData[i];
		char ch = 0;
		switch (subMode) {
		case Mode::ALPHA:
			// Alpha (uppercase alphabetic)
			if (subModeCh < 26) {
				// Upper case Alpha Character
				ch = (char)('A' + subModeCh);
			}
			else {
				if (subModeCh == 26) {
					ch = ' ';
				}
				else if (subModeCh == LL) {
					subMode = Mode::LOWER;
				}
				else if (subModeCh == ML) {
					subMode = Mode::MIXED;
				}
				else if (subModeCh == PS) {
					// Shift to punctuation
					priorToShiftMode = subMode;
					subMode = Mode::PUNCT_SHIFT;
				}
				else if (subModeCh == MODE_SHIFT_TO_BYTE_COMPACTION_MODE) {
					result.appendUtf8((char)byteCompactionData[i]);
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;

		case Mode::LOWER:
			// Lower (lowercase alphabetic)
			if (subModeCh < 26) {
				ch = (char)('a' + subModeCh);
			}
			else {
				if (subModeCh == 26) {
					ch = ' ';
				}
				else if (subModeCh == AS) {
					// Shift to alpha
					priorToShiftMode = subMode;
					subMode = Mode::ALPHA_SHIFT;
				}
				else if (subModeCh == ML) {
					subMode = Mode::MIXED;
				}
				else if (subModeCh == PS) {
					// Shift to punctuation
					priorToShiftMode = subMode;
					subMode = Mode::PUNCT_SHIFT;
				}
				else if (subModeCh == MODE_SHIFT_TO_BYTE_COMPACTION_MODE) {
					// TODO Does this need to use the current character encoding? See other occurrences below
					result.appendUtf8((char)byteCompactionData[i]);
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;

		case Mode::MIXED:
			// Mixed (numeric and some punctuation)
			if (subModeCh < PL) {
				ch = MIXED_CHARS[subModeCh];
			}
			else {
				if (subModeCh == PL) {
					subMode = Mode::PUNCT;
				}
				else if (subModeCh == 26) {
					ch = ' ';
				}
				else if (subModeCh == LL) {
					subMode = Mode::LOWER;
				}
				else if (subModeCh == AL) {
					subMode = Mode::ALPHA;
				}
				else if (subModeCh == PS) {
					// Shift to punctuation
					priorToShiftMode = subMode;
					subMode = Mode::PUNCT_SHIFT;
				}
				else if (subModeCh == MODE_SHIFT_TO_BYTE_COMPACTION_MODE) {
					result.appendUtf8((char)byteCompactionData[i]);
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;

		case Mode::PUNCT:
			// Punctuation
			if (subModeCh < PAL) {
				ch = PUNCT_CHARS[subModeCh];
			}
			else {
				if (subModeCh == PAL) {
					subMode = Mode::ALPHA;
				}
				else if (subModeCh == MODE_SHIFT_TO_BYTE_COMPACTION_MODE) {
					result.appendUtf8((char)byteCompactionData[i]);
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;

		case Mode::ALPHA_SHIFT:
			// Restore sub-mode
			subMode = priorToShiftMode;
			if (subModeCh < 26) {
				ch = (char)('A' + subModeCh);
			}
			else {
				if (subModeCh == 26) {
					ch = ' ';
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;

		case Mode::PUNCT_SHIFT:
			// Restore sub-mode
			subMode = priorToShiftMode;
			if (subModeCh < PAL) {
				ch = PUNCT_CHARS[subModeCh];
			}
			else {
				if (subModeCh == PAL) {
					subMode = Mode::ALPHA;
				}
				else if (subModeCh == MODE_SHIFT_TO_BYTE_COMPACTION_MODE) {
					// PS before Shift-to-Byte is used as a padding character, 
					// see 5.4.2.4 of the specification
					result.appendUtf8((char)byteCompactionData[i]);
				}
				else if (subModeCh == TEXT_COMPACTION_MODE_LATCH) {
					subMode = Mode::ALPHA;
				}
			}
			break;
		}
		if (ch != 0) {
			// Append decoded character to result
			result.appendUtf8(ch);
		}
		i++;
	}
}

/**
* Text Compaction mode (see 5.4.1.5) permits all printable ASCII characters to be
* encoded, i.e. values 32 - 126 inclusive in accordance with ISO/IEC 646 (IRV), as
* well as selected control characters.
*
* @param codewords The array of codewords (data + error)
* @param codeIndex The current index into the codeword array.
* @param result    The decoded data is appended to the result.
* @return The next index into the codeword array.
*/
static int TextCompaction(const std::vector<int>& codewords, int codeIndex, String& result)
{
	// 2 character per codeword
	std::vector<int> textCompactionData((codewords[0] - codeIndex) * 2, 0);
	// Used to hold the byte compaction value if there is a mode shift
	std::vector<int> byteCompactionData((codewords[0] - codeIndex) * 2, 0);

	int index = 0;
	bool end = false;
	while ((codeIndex < codewords[0]) && !end) {
		int code = codewords[codeIndex++];
		if (code < TEXT_COMPACTION_MODE_LATCH) {
			textCompactionData[index] = code / 30;
			textCompactionData[index + 1] = code % 30;
			index += 2;
		}
		else {
			switch (code) {
			case TEXT_COMPACTION_MODE_LATCH:
				// reinitialize text compaction mode to alpha sub mode
				textCompactionData[index++] = TEXT_COMPACTION_MODE_LATCH;
				break;
			case BYTE_COMPACTION_MODE_LATCH:
			case BYTE_COMPACTION_MODE_LATCH_6:
			case NUMERIC_COMPACTION_MODE_LATCH:
			case BEGIN_MACRO_PDF417_CONTROL_BLOCK:
			case BEGIN_MACRO_PDF417_OPTIONAL_FIELD:
			case MACRO_PDF417_TERMINATOR:
				codeIndex--;
				end = true;
				break;
			case MODE_SHIFT_TO_BYTE_COMPACTION_MODE:
				// The Mode Shift codeword 913 shall cause a temporary
				// switch from Text Compaction mode to Byte Compaction mode.
				// This switch shall be in effect for only the next codeword,
				// after which the mode shall revert to the prevailing sub-mode
				// of the Text Compaction mode. Codeword 913 is only available
				// in Text Compaction mode; its use is described in 5.4.2.4.
				textCompactionData[index] = MODE_SHIFT_TO_BYTE_COMPACTION_MODE;
				code = codewords[codeIndex++];
				byteCompactionData[index] = code;
				index++;
				break;
			}
		}
	}
	DecodeTextCompaction(textCompactionData, byteCompactionData, index, result);
	return codeIndex;
}


/**
* Byte Compaction mode (see 5.4.3) permits all 256 possible 8-bit byte values to be encoded.
* This includes all ASCII characters value 0 to 127 inclusive and provides for international
* character set support.
*
* @param mode      The byte compaction mode i.e. 901 or 924
* @param codewords The array of codewords (data + error)
* @param encoding  Currently active character encoding
* @param codeIndex The current index into the codeword array.
* @param result    The decoded data is appended to the result.
* @return The next index into the codeword array.
*/
static int ByteCompaction(int mode, const std::vector<int>& codewords, CharacterSet encoding, int codeIndex, String& result)
{
	ByteArray decodedBytes;
	if (mode == BYTE_COMPACTION_MODE_LATCH) {
		// Total number of Byte Compaction characters to be encoded
		// is not a multiple of 6
		int count = 0;
		long value = 0;
		std::array<int, 6> byteCompactedCodewords = {};
		bool end = false;
		int nextCode = codewords[codeIndex++];
		while ((codeIndex < codewords[0]) && !end) {
			byteCompactedCodewords[count++] = nextCode;
			// Base 900
			value = 900 * value + nextCode;
			nextCode = codewords[codeIndex++];
			// perhaps it should be ok to check only nextCode >= TEXT_COMPACTION_MODE_LATCH
			if (nextCode == TEXT_COMPACTION_MODE_LATCH ||
				nextCode == BYTE_COMPACTION_MODE_LATCH ||
				nextCode == NUMERIC_COMPACTION_MODE_LATCH ||
				nextCode == BYTE_COMPACTION_MODE_LATCH_6 ||
				nextCode == BEGIN_MACRO_PDF417_CONTROL_BLOCK ||
				nextCode == BEGIN_MACRO_PDF417_OPTIONAL_FIELD ||
				nextCode == MACRO_PDF417_TERMINATOR) {
				codeIndex--;
				end = true;
			}
			else {
				if ((count % 5 == 0) && (count > 0)) {
					// Decode every 5 codewords
					// Convert to Base 256
					for (int j = 0; j < 6; ++j) {
						decodedBytes.push_back((uint8_t)(value >> (8 * (5 - j))));
					}
					value = 0;
					count = 0;
				}
			}
		}

		// if the end of all codewords is reached the last codeword needs to be added
		if (codeIndex == codewords[0] && nextCode < TEXT_COMPACTION_MODE_LATCH) {
			byteCompactedCodewords[count++] = nextCode;
		}

		// If Byte Compaction mode is invoked with codeword 901,
		// the last group of codewords is interpreted directly
		// as one byte per codeword, without compaction.
		for (int i = 0; i < count; i++) {
			decodedBytes.push_back((uint8_t)byteCompactedCodewords[i]);
		}

	}
	else if (mode == BYTE_COMPACTION_MODE_LATCH_6) {
		// Total number of Byte Compaction characters to be encoded
		// is an integer multiple of 6
		int count = 0;
		long value = 0;
		bool end = false;
		while (codeIndex < codewords[0] && !end) {
			int code = codewords[codeIndex++];
			if (code < TEXT_COMPACTION_MODE_LATCH) {
				count++;
				// Base 900
				value = 900 * value + code;
			}
			else {
				if (code == TEXT_COMPACTION_MODE_LATCH ||
					code == BYTE_COMPACTION_MODE_LATCH ||
					code == NUMERIC_COMPACTION_MODE_LATCH ||
					code == BYTE_COMPACTION_MODE_LATCH_6 ||
					code == BEGIN_MACRO_PDF417_CONTROL_BLOCK ||
					code == BEGIN_MACRO_PDF417_OPTIONAL_FIELD ||
					code == MACRO_PDF417_TERMINATOR) {
					codeIndex--;
					end = true;
				}
			}
			if ((count % 5 == 0) && (count > 0)) {
				// Decode every 5 codewords
				// Convert to Base 256
				for (int j = 0; j < 6; ++j) {
					decodedBytes.push_back((uint8_t)(value >> (8 * (5 - j))));
				}
				value = 0;
				count = 0;
			}
		}
	}
	result += StringCodecs::Instance()->toUnicode(decodedBytes.data(), decodedBytes.length(), encoding);
	return codeIndex;
}


/**
* Convert a list of Numeric Compacted codewords from Base 900 to Base 10.
*
* @param codewords The array of codewords
* @param count     The number of codewords
* @return The decoded string representing the Numeric data.
*/
/*
EXAMPLE
Encode the fifteen digit numeric string 000213298174000
Prefix the numeric string with a 1 and set the initial value of
t = 1 000 213 298 174 000
Calculate codeword 0
d0 = 1 000 213 298 174 000 mod 900 = 200

t = 1 000 213 298 174 000 div 900 = 1 111 348 109 082
Calculate codeword 1
d1 = 1 111 348 109 082 mod 900 = 282

t = 1 111 348 109 082 div 900 = 1 234 831 232
Calculate codeword 2
d2 = 1 234 831 232 mod 900 = 632

t = 1 234 831 232 div 900 = 1 372 034
Calculate codeword 3
d3 = 1 372 034 mod 900 = 434

t = 1 372 034 div 900 = 1 524
Calculate codeword 4
d4 = 1 524 mod 900 = 624

t = 1 524 div 900 = 1
Calculate codeword 5
d5 = 1 mod 900 = 1
t = 1 div 900 = 0
Codeword sequence is: 1, 624, 434, 632, 282, 200

Decode the above codewords involves
1 x 900 power of 5 + 624 x 900 power of 4 + 434 x 900 power of 3 +
632 x 900 power of 2 + 282 x 900 power of 1 + 200 x 900 power of 0 = 1000213298174000

Remove leading 1 =>  Result is 000213298174000
*/
static ErrorStatus DecodeBase900toBase10(const std::vector<int>& codewords, int count, std::string& resultString)
{
	BigInteger result;
	for (int i = 0; i < count; i++) {
		result = result + (EXP900()[count - i - 1] * codewords[i]);
	}
	resultString = result.toString();
	if (!resultString.empty() && resultString.front() == '1') {
		resultString = resultString.substr(1);
		return ErrorStatus::NoError;
	}
	return ErrorStatus::FormatError;
}


/**
* Numeric Compaction mode (see 5.4.4) permits efficient encoding of numeric data strings.
*
* @param codewords The array of codewords (data + error)
* @param codeIndex The current index into the codeword array.
* @param result    The decoded data is appended to the result.
* @return The next index into the codeword array.
*/
static ErrorStatus NumericCompaction(const std::vector<int>& codewords, int codeIndex, String& result, int& next)
{
	int count = 0;
	bool end = false;

	std::vector<int> numericCodewords(MAX_NUMERIC_CODEWORDS);

	while (codeIndex < codewords[0] && !end) {
		int code = codewords[codeIndex++];
		if (codeIndex == codewords[0]) {
			end = true;
		}
		if (code < TEXT_COMPACTION_MODE_LATCH) {
			numericCodewords[count] = code;
			count++;
		}
		else {
			if (code == TEXT_COMPACTION_MODE_LATCH ||
				code == BYTE_COMPACTION_MODE_LATCH ||
				code == BYTE_COMPACTION_MODE_LATCH_6 ||
				code == BEGIN_MACRO_PDF417_CONTROL_BLOCK ||
				code == BEGIN_MACRO_PDF417_OPTIONAL_FIELD ||
				code == MACRO_PDF417_TERMINATOR) {
				codeIndex--;
				end = true;
			}
		}
		if (count % MAX_NUMERIC_CODEWORDS == 0 || code == NUMERIC_COMPACTION_MODE_LATCH || end) {
			// Re-invoking Numeric Compaction mode (by using codeword 902
			// while in Numeric Compaction mode) serves  to terminate the
			// current Numeric Compaction mode grouping as described in 5.4.4.2,
			// and then to start a new one grouping.
			if (count > 0) {
				std::string tmp;
				auto status = DecodeBase900toBase10(numericCodewords, count, tmp);
				if (StatusIsError(status)) {
					return status;
				}
				result.appendUtf8(tmp);
				count = 0;
			}
		}
	}
	next = codeIndex;
	return ErrorStatus::NoError;
}


static ErrorStatus DecodeMacroBlock(const std::vector<int>& codewords, int codeIndex, DecoderResultExtra& resultMetadata, int& next)
{
	if (codeIndex + NUMBER_OF_SEQUENCE_CODEWORDS > codewords[0]) {
		// we must have at least two bytes left for the segment index
		return ErrorStatus::FormatError;
	}
	std::vector<int> segmentIndexArray(NUMBER_OF_SEQUENCE_CODEWORDS);
	for (int i = 0; i < NUMBER_OF_SEQUENCE_CODEWORDS; i++, codeIndex++) {
		segmentIndexArray[i] = codewords[codeIndex];
	}

	std::string strBuf;
	ErrorStatus status = DecodeBase900toBase10(segmentIndexArray, NUMBER_OF_SEQUENCE_CODEWORDS, strBuf);
	if (StatusIsError(status)) {
		return status;
	}

	resultMetadata.setSegmentIndex(std::stoi(strBuf));

	String fileId;
	codeIndex = TextCompaction(codewords, codeIndex, fileId);
	resultMetadata.setFileId(fileId);

	if (codewords[codeIndex] == BEGIN_MACRO_PDF417_OPTIONAL_FIELD) {
		codeIndex++;
		std::vector<int> additionalOptionCodeWords;
		additionalOptionCodeWords.reserve(codewords[0] - codeIndex);

		bool end = false;
		while ((codeIndex < codewords[0]) && !end) {
			int code = codewords[codeIndex++];
			if (code < TEXT_COMPACTION_MODE_LATCH) {
				additionalOptionCodeWords.push_back(code);
			}
			else {
				switch (code) {
				case MACRO_PDF417_TERMINATOR:
					resultMetadata.setLastSegment(true);
					codeIndex++;
					end = true;
					break;
				default:
					return ErrorStatus::FormatError;
				}
			}
		}

		resultMetadata.setOptionalData(additionalOptionCodeWords);
	}
	else if (codewords[codeIndex] == MACRO_PDF417_TERMINATOR) {
		resultMetadata.setLastSegment(true);
		codeIndex++;
	}

	next = codeIndex;
	return ErrorStatus::NoError;
}

ErrorStatus
DecodedBitStreamParser::Decode(const std::vector<int>& codewords, int ecLevel, DecoderResult& result)
{
	String resultString;
	auto encoding = DEFAULT_ENCODING;
	// Get compaction mode
	int codeIndex = 1;
	int code = codewords[codeIndex++];
	auto resultMetadata = std::make_shared<DecoderResultExtra>();
	ErrorStatus status = ErrorStatus::NoError;
	while (codeIndex < codewords[0] && status == ErrorStatus::NoError) {
		switch (code) {
		case TEXT_COMPACTION_MODE_LATCH:
			codeIndex = TextCompaction(codewords, codeIndex, resultString);
			break;
		case BYTE_COMPACTION_MODE_LATCH:
		case BYTE_COMPACTION_MODE_LATCH_6:
			codeIndex = ByteCompaction(code, codewords, encoding, codeIndex, resultString);
			break;
		case MODE_SHIFT_TO_BYTE_COMPACTION_MODE:
			resultString.appendUtf8((char)codewords[codeIndex++]);
			break;
		case NUMERIC_COMPACTION_MODE_LATCH:
			status = NumericCompaction(codewords, codeIndex, resultString, codeIndex);
			break;
		case ECI_CHARSET:
			encoding = CharacterSetECI::CharsetFromValue(codewords[codeIndex++]);
			break;
		case ECI_GENERAL_PURPOSE:
			// Can't do anything with generic ECI; skip its 2 characters
			codeIndex += 2;
			break;
		case ECI_USER_DEFINED:
			// Can't do anything with user ECI; skip its 1 character
			codeIndex++;
			break;
		case BEGIN_MACRO_PDF417_CONTROL_BLOCK:
			status = DecodeMacroBlock(codewords, codeIndex, *resultMetadata, codeIndex);
			break;
		case BEGIN_MACRO_PDF417_OPTIONAL_FIELD:
		case MACRO_PDF417_TERMINATOR:
			// Should not see these outside a macro block
			status = ErrorStatus::FormatError;
			break;
		default:
			// Default to text compaction. During testing numerous barcodes
			// appeared to be missing the starting mode. In these cases defaulting
			// to text compaction seems to work.
			codeIndex--;
			codeIndex = TextCompaction(codewords, codeIndex, resultString);
			break;
		}
		if (codeIndex < (int)codewords.size()) {
			code = codewords[codeIndex++];
		}
		else {
			status = ErrorStatus::FormatError;
		}
	}
	if (resultString.empty()) {
		status = ErrorStatus::FormatError;
	}

	if (StatusIsOK(status)) {
		result.setText(resultString);
		result.setEcLevel(std::to_string(ecLevel));
		result.setExtra(resultMetadata);
	}
	return status;
}


} // Pdf417
} // ZXing