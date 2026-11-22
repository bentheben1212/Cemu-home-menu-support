#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/CafeProcessState.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/CafeProcess.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/gx2/GX2_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/LaunchSettings.h"
#include <pugixml.hpp>

sint32 _SYSGetUPIDFromTitleId(uint64 titleId); // sysapp.cpp

namespace coreinit
{
	sint32 ppc_vcprintf_pad(char* strOut, sint32 maxLength, sint32 padLength, char padChar)
	{
		padLength = std::min<sint32>(padLength, maxLength);
		sint32 paddedLength = padLength;
		while ( padLength > 0)
		{
			*strOut = padChar;
			strOut++;
			padLength--;
		}
		return paddedLength;
	}

	// handler for %s
	sint32 ppc_vcprintf_str(char* strOut, sint32 maxLength, MEMPTR<char> strPointer, std::optional<sint32> width, std::optional<sint32> precision)
	{
		if (maxLength <= 0)
			return 0;
		const char* stringData = "(null)";
		if (strPointer)
			stringData = strPointer.GetPtr();
		else
		{
			if (precision.has_value() && *precision > 6)
				precision = 6;
		}
		// get string length
		sint32 stringLength = 0;
		if (precision.has_value())
		{
			stringLength = *precision;
			for (sint32 i=0; i<stringLength; i++)
			{
				if (stringData[i] == '\0')
				{
					stringLength = i;
					break;
				}
			}
		}
		else
			stringLength = strlen(stringData);
		if (stringLength < 0)
		{
			cemuLog_log(LogType::APIErrors, "ppc_vprintf: String length less than zero");
			stringLength = 0;
		}
		sint32 outputLength = 0;
		if (width.has_value() && width.value() > 0)
		{
			// left padding
			sint32 paddedLength = *width;
			paddedLength -= stringLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		stringLength = std::min<sint32>(stringLength, maxLength);
		cemu_assert(stringLength >= 0);
		memcpy(strOut, stringData, stringLength);
		outputLength += stringLength;
		if (width.has_value() && width.value() < 0)
		{
			sint32 paddedLength = -*width;
			paddedLength -= stringLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength - stringLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut + stringLength, maxLength - stringLength, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	// handler for %c
	sint32 ppc_vcprintf_char(char* strOut, sint32 maxLength, char character, std::optional<sint32> width, std::optional<sint32> precision)
	{
		if (maxLength <= 0)
			return 0;
		sint32 outputLength = 0;
		if (width.has_value() && width.value() > 0)
		{
			// left padding
			sint32 paddedLength = *width;
			paddedLength -= 1;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		if (maxLength > 0)
		{
			*strOut = character;
			outputLength++;
		}
		if (width.has_value() && width.value() < 0)
		{
			sint32 paddedLength = -*width;
			paddedLength -= 1;
			paddedLength = std::min<sint32>(paddedLength, maxLength - 1);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut + 1, maxLength - 1, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	char GetHexDigit(uint8 nibble, bool isUppercase)
	{
		if (nibble >= 10)
		{
			return isUppercase ? (nibble - 10 + 'A') : (nibble - 10 + 'a');
		}
		return nibble + '0';
	}

	// handler for integers
	sint32 ppc_vcprintf_integer(char* strOut, sint32 maxLength, bool isNegative, uint64 absValue, bool isHex, bool uppercase, bool alternateForm, std::optional<sint32> width, std::optional<sint32> precision, char padChar)
	{
		if (maxLength <= 0)
			return 0;

		// convert to digits string
		char digitBuf[64];
		sint32 digitCount = 0;
		uint64 tmpValue = absValue;
		if (isHex)
		{
			do
			{
				digitBuf[digitCount++] = GetHexDigit(tmpValue&0xF, uppercase);
				tmpValue >>= 4;
			}
			while (tmpValue != 0);
		}
		else
		{
			do
			{
				digitBuf[digitCount++] = (char)('0' + (tmpValue % 10));
				tmpValue /= 10;
			}
			while (tmpValue != 0);
		}

		// handle "%.0d"
		if (precision.has_value() && *precision == 0 && (!isNegative && absValue == 0))
			digitCount = 0;

		// precision adds leading zeroes for digits only
		sint32 precisionZeroes = 0;
		if (precision.has_value() && *precision > digitCount)
			precisionZeroes = *precision - digitCount;

		sint32 numberLength = digitCount + precisionZeroes;
		if (isNegative)
			numberLength++; // count the sign
		if (isHex && alternateForm && maxLength >= 2 && absValue != 0)
			numberLength += 2;

		sint32 outputLength = 0;

		// zero padding is a special case because its between the sign and the number
		bool widthZeroPad = width.has_value() && *width > 0 && padChar == '0' && !precision.has_value();

		// handle space padding left
		if (width.has_value() && *width > 0 && !widthZeroPad)
		{
			sint32 paddedLength = *width - numberLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}

		// sign
		if (isNegative && maxLength > 0)
		{
			*strOut = '-';
			strOut++;
			maxLength--;
			outputLength++;
		}
		// hex alternate form prefix (0x / 0X)
		bool hasHexPrefix = false;
		if (isHex && alternateForm && maxLength >= 2 && absValue != 0)
		{
			strOut[0] = '0';
			if (uppercase)
				strOut[1] = 'X';
			else
				strOut[1] = 'x';
			strOut += 2;
			maxLength -= 2;
			outputLength += 2;
			hasHexPrefix = true;
		}
		// output zero padding after sign
		if (widthZeroPad)
		{
			sint32 paddedLength = *width - (digitCount + (isNegative ? 1 : 0));
			if (hasHexPrefix)
				paddedLength -= 2;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, '0');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		else if (precisionZeroes > 0)
		{
			// precision zeroes
			sint32 paddedLength = std::min<sint32>(precisionZeroes, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, '0');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}

		// digits (the buffer is in reverse order)
		sint32 writableDigits = std::min<sint32>(digitCount, maxLength);
		for (sint32 i = 0; i < writableDigits; i++)
			strOut[i] = digitBuf[digitCount - 1 - i];
		strOut += writableDigits;
		maxLength -= writableDigits;
		outputLength += writableDigits;

		// space padding right
		if (width.has_value() && *width < 0)
		{
			sint32 paddedLength = (-*width) - numberLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	// handler for signed decimal integers (64-bit core)
	sint32 ppc_vcprintf_decimal64(char* strOut, sint32 maxLength, bool isNegative, uint64 absValue, std::optional<sint32> width, std::optional<sint32> precision, char padChar)
	{
		return ppc_vcprintf_integer(strOut, maxLength, isNegative, absValue, false, false, false, width, precision, padChar);
	}

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs)
	{
		if (maxLength <= 0)
			return 0;
		char tempStr[4096];
		sint32 writeIndex = 0;
		while (*formatStr)
		{
			char c = *formatStr;
			if (c == '%')
			{
				const char* formatStart = formatStr;
				formatStr++;
				if (*formatStr == '%')
				{
					// percent sign
					if (writeIndex >= maxLength)
						break;
					strOut[writeIndex] = '%';
					writeIndex++;
					formatStr++;
					continue;
				}
				// flags
				char padChar = ' ';
				bool alternateForm = false;
				bool flag_leftJustify = false;
				if (*formatStr == '-')
				{
					flag_leftJustify = true;
					formatStr++;
				}
				if (*formatStr == '+')
				{
					formatStr++;
				}
				if (*formatStr == ' ')
				{
					formatStr++;
				}
				if (*formatStr == '#')
				{
					alternateForm = true;
					formatStr++;
				}
				if (*formatStr == '0')
				{
					padChar = '0';
					formatStr++;
				}
				// width
				std::optional<sint32> width;
				if (*formatStr == '*')
				{
					formatStr++;
					width = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					if (flag_leftJustify)
						width = -abs(*width);
				}
				else
				{
					width = 0;
					while (*formatStr >= '0' && *formatStr <= '9')
					{
						*width *= 10;
						*width += (*formatStr - '0');
						formatStr++;
					}
					if (flag_leftJustify)
						width = -*width;
				}
				// precision
				std::optional<sint32> precision;
				if (*formatStr == '.')
				{
					formatStr++;
					if (*formatStr == '*')
					{
						precision = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
						if (*precision < 0)
							precision.reset();
						formatStr++;
					}
					else
					{
						precision = 0;
						while (*formatStr >= '0' && *formatStr <= '9')
						{
							*precision *= 10;
							*precision += (*formatStr - '0');
							formatStr++;
						}
					}
				}
				// length + specifier
				char tempFormat[64];
				if (strncmp(formatStr, "lld", 3) == 0 || strncmp(formatStr, "lli", 3) == 0)
				{
					// 64bit decimal (signed)
					formatStr += 3;
					sint64 v = *(sint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					if (v >= 0)
						writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, width, precision, padChar);
					else
						writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, true, ~(uint64)v + 1, width, precision, padChar);
				}
				else if (strncmp(formatStr, "llu", 3) == 0)
				{
					// 64bit decimal (unsigned)
					formatStr += 3;
					uint64 v = *(uint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, v, width, precision, padChar);
				}
				else if (*formatStr == 'd' || *formatStr == 'i' || strncmp(formatStr, "ld", 2) == 0 || strncmp(formatStr, "li", 2) == 0)
				{
					// 32bit decimal (signed)
					if (formatStr[0] == 'l')
						formatStr += 2;
					else
						formatStr++;
					sint32 v = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, v < 0, (uint64)abs((sint64)v), width, precision, padChar);
				}
				else if (*formatStr == 'u' || strncmp(formatStr, "lu", 2) == 0)
				{
					// 32bit decimal (unsigned)
					if (formatStr[0] == 'l')
						formatStr += 2;
					else
						formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, width, precision, padChar);
				}
				else if (strncmp(formatStr, "llx", 3) == 0 || strncmp(formatStr, "llX", 3) == 0)
				{
					// 64bit hexadecimal
					bool isUppercase = formatStr[2] == 'X';
					formatStr += 3;
					uint64 v = *(uint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, isUppercase, alternateForm, width, precision, padChar);
				}
				else if (*formatStr == 'x' || *formatStr == 'X')
				{
					// 32bit hexadecimal
					bool isUppercase = *formatStr == 'X';
					formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, isUppercase, alternateForm, width, precision, padChar);
				}
				else if (*formatStr == 'p')
				{
					// pointer
					formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					if (!precision.has_value())
						precision = 8;
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, false, true, width, precision, padChar);
				}
				else if (*formatStr == 's')
				{
					// string
					formatStr++;
					MPTR strOffset = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_str(strOut + writeIndex, maxLength - writeIndex, MEMPTR<char>(strOffset), width, precision);
				}
				else if (*formatStr == 'c')
				{
					// character
					formatStr++;
					char character = (char)(uint32)*(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_char(strOut + writeIndex, maxLength - writeIndex, character, width, precision);
				}
				else if (*formatStr == 'f' || *formatStr == 'g' || *formatStr == 'G')
				{
					formatStr++;
					strncpy(tempFormat, formatStart, std::min((std::ptrdiff_t)sizeof(tempFormat) - 1, formatStr - formatStart));
					if ((formatStr - formatStart) < sizeof(tempFormat))
						tempFormat[(formatStr - formatStart)] = '\0';
					else
						tempFormat[sizeof(tempFormat) - 1] = '\0';
					sint32 tempLen = sprintf(tempStr, tempFormat, (double)*(betype<double>*)_ppc_va_arg(vargs, ppc_va_type::FLOAT_OR_DOUBLE));
					for (sint32 i = 0; i < tempLen; i++)
					{
						if (writeIndex >= maxLength)
							break;
						strOut[writeIndex] = tempStr[i];
						writeIndex++;
					}
				}
				else if (formatStr[0] == 'l' && formatStr[1] == 'f')
				{
					// double
					formatStr += 2;
					strncpy(tempFormat, formatStart, std::min((std::ptrdiff_t)sizeof(tempFormat) - 1, formatStr - formatStart));
					if ((formatStr - formatStart) < sizeof(tempFormat))
						tempFormat[(formatStr - formatStart)] = '\0';
					else
						tempFormat[sizeof(tempFormat) - 1] = '\0';
					sint32 tempLen = sprintf(tempStr, tempFormat, (double)*(betype<double>*)_ppc_va_arg(vargs, ppc_va_type::FLOAT_OR_DOUBLE));
					for (sint32 i = 0; i < tempLen; i++)
					{
						if (writeIndex >= maxLength)
							break;
						strOut[writeIndex] = tempStr[i];
						writeIndex++;
					}
				}
				else
				{
					// unsupported / unknown specifier
					cemu_assert_debug(false);
					break;
				}
				cemu_assert(writeIndex <= maxLength); // sanity check
			}
			else
			{
				if (writeIndex >= maxLength)
					break;
				strOut[writeIndex] = c;
				writeIndex++;
				formatStr++;
			}
		}
		writeIndex = std::min<sint32>(writeIndex, maxLength-1);
		strOut[writeIndex] = '\0';
		return writeIndex;
	}

	/* coreinit logging and string format */

	sint32 __os_snprintf(char* outputStr, sint32 maxLength, const char* formatStr)
	{
		ppc_define_va_list(3, 0);
		sint32 r = ppc_vprintf(formatStr, outputStr, maxLength, &vargs);
		return r;
	}

	enum class CafeLogType
	{
		OSCONSOLE = 0,
	};

	struct CafeLogBuffer
	{
		std::array<char, 270> lineBuffer;
		size_t lineLength{};
	};

	CafeLogBuffer g_logBuffer_OSReport;

	CafeLogBuffer& getLogBuffer(CafeLogType cafeLogType)
	{
		if (cafeLogType == CafeLogType::OSCONSOLE)
			return g_logBuffer_OSReport;
		// default to OSReport
		return g_logBuffer_OSReport;
	}

	std::string_view getLogBufferName(CafeLogType cafeLogType)
	{
		if (cafeLogType == CafeLogType::OSCONSOLE)
			return "OSConsole";
		return "Unknown";
	}

	std::mutex sCafeConsoleMutex;
	bool s_forwardConsoleLogs;
	std::deque<std::string> s_recentConsoleLines; // last lines written to the console, dumped on OSPanic
	static constexpr size_t RECENT_CONSOLE_LINES = 32;

	// writes the most recent console output to the log, even if console logging is disabled
	void __OSDumpRecentConsoleOutput()
	{
		std::unique_lock _l(sCafeConsoleMutex);
		cemuLog_log(LogType::Force, "Recent console output (last {} lines):", s_recentConsoleLines.size());
		for (auto& line : s_recentConsoleLines)
			cemuLog_log(LogType::Force, "  {}", line);
		CafeLogBuffer& logBuffer = g_logBuffer_OSReport;
		if (logBuffer.lineLength > 0)
			cemuLog_log(LogType::Force, "  {}", std::string_view(logBuffer.lineBuffer.data(), logBuffer.lineLength));
	}
	
	void WriteCafeConsole(CafeLogType cafeLogType, const char* msg, sint32 len)
	{
		std::unique_lock _l(sCafeConsoleMutex);
		CafeLogBuffer& logBuffer = getLogBuffer(cafeLogType);
		// once a line is full or \n is written it will be posted to log
		auto flushLine = [](CafeLogBuffer& cafeLogBuffer, std::string_view cafeLogName) -> void
		{
			cemuLog_log(LogType::CoreinitLogging, "[{0}] {1}", cafeLogName, std::basic_string_view(cafeLogBuffer.lineBuffer.data(), cafeLogBuffer.lineLength));
			s_recentConsoleLines.emplace_back(cafeLogBuffer.lineBuffer.data(), cafeLogBuffer.lineLength);
			if (s_recentConsoleLines.size() > RECENT_CONSOLE_LINES)
				s_recentConsoleLines.pop_front();
			cafeLogBuffer.lineLength = 0;
		};

		if (s_forwardConsoleLogs) {
			if (cafeLogType == CafeLogType::OSCONSOLE) {
				fwrite(msg, 1, len, stdout);
				fflush(stdout);
			} else {
				fwrite(msg, 1, len, stderr);
			}
		}
		while (len)
		{
			char c = *msg;
			msg++;
			len--;
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				// flush line immediately
				flushLine(logBuffer, getLogBufferName(cafeLogType));
				continue;
			}
			logBuffer.lineBuffer[logBuffer.lineLength] = c;
			logBuffer.lineLength++;
			if (logBuffer.lineLength >= logBuffer.lineBuffer.size())
				flushLine(logBuffer, getLogBufferName(cafeLogType));
		}
	}

	void COSVReport(COSReportModule module, COSReportLevel level, const char* format, ppc_va_list* vargs)
	{
		char tmpBuffer[1024];
		sint32 len = ppc_vprintf(format, tmpBuffer, sizeof(tmpBuffer), vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len);
	}

	void OSReport(const char* format)
	{
		ppc_define_va_list(1, 0);
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, &vargs);
	}

	void OSVReport(const char* format, ppc_va_list* vargs)
	{
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, vargs);
	}

	void COSWarn(int moduleId, const char* format)
	{
		ppc_define_va_list(2, 0);
		char tmpBuffer[1024];
		int prefixLen = sprintf(tmpBuffer, "[COSWarn-%d] ", moduleId);
		sint32 len = ppc_vprintf(format, tmpBuffer + prefixLen, sizeof(tmpBuffer) - prefixLen, &vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len + prefixLen);
	}

	void OSLogPrintf(int ukn1, int ukn2, int ukn3, const char* format)
	{
		ppc_define_va_list(4, 0);
		char tmpBuffer[1024];
		int prefixLen = sprintf(tmpBuffer, "[OSLogPrintf-%d-%d-%d] ", ukn1, ukn2, ukn3);
		sint32 len = ppc_vprintf(format, tmpBuffer + prefixLen, sizeof(tmpBuffer) - prefixLen, &vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len + prefixLen);
	}

	void OSConsoleWrite(const char* strPtr, sint32 length)
	{
		if (length < 0)
			return;
		WriteCafeConsole(CafeLogType::OSCONSOLE, strPtr, length);
	}

	void OSFatal(const char* msg)
	{
		// print to log
		ppc_define_va_list(1, 0);
		COSVReport(COSReportModule::coreinit, COSReportLevel::Error, msg, &vargs);

		// print to screen
		OSScreenInit();
		uint32 tvBufferSize = OSScreenGetBufferSizeEx(OSSCREEN_TV);
		OSScreenSetBufferEx(OSSCREEN_TV, MEMORY_MEM1_AREA_ADDR);
		OSScreenSetBufferEx(OSSCREEN_DRC, MEMORY_MEM1_AREA_ADDR + tvBufferSize);
		OSScreenEnableEx(OSSCREEN_TV, 1);
		OSScreenEnableEx(OSSCREEN_DRC, 1);

		while ( true )
		{
			OSScreenClearBufferEx(OSSCREEN_TV, 0);
			OSScreenClearBufferEx(OSSCREEN_DRC, 0);
			OSScreenPutFontEx(OSSCREEN_TV, 0, 0, msg);
			OSScreenPutFontEx(OSSCREEN_DRC, 0, 0, msg);
			OSScreenFlipBuffersEx(OSSCREEN_TV);
			OSScreenFlipBuffersEx(OSSCREEN_DRC);
			OSSleepTicks(ESPRESSO_TIMER_CLOCK / 30);
		}
	}

	/* home button menu */

	bool g_homeButtonMenuEnabled = false;

	bool OSIsHomeButtonMenuEnabled()
	{
		return g_homeButtonMenuEnabled;
	}

	bool OSEnableHomeButtonMenu(bool enable)
	{
		g_homeButtonMenuEnabled = enable;
		return true;
	}

	uint32 OSGetPFID()
	{
		// the title's own PFID/UPID: 2 for the Wii U Menu, the applet ID for applets (e.g. 5 for the HOME Menu), 15 for games
		sint32 pfid = _SYSGetUPIDFromTitleId(CafeSystem::GetForegroundTitleId());
		return pfid >= 0 ? (uint32)pfid : 15;
	}

	uint32 OSGetUPID()
	{
		return OSGetPFID();
	}

	uint64 s_currentTitleId;

	uint64 OSGetTitleID()
	{
		return s_currentTitleId;
	}

	uint32 s_sdkVersion;

	uint32 __OSGetProcessSDKVersion()
	{
		return s_sdkVersion;
	}

	void OSLauncherThread(uint64 titleId)
	{
		if (CafeProcess::HasSuspendedTitle())
		{
			if (titleId == CafeProcess::GetSuspendedTitleId())
			{
				// back to the suspended title (e.g. the HOME Menu closed): it continues where it was
				CafeSystem::ShutdownTitleForSwitch();
				CafeSystem::RequestRecreateCanvas();
				if (CafeProcess::ResumeSuspendedTitle())
					return;
				cemuLog_log(LogType::Force, "Process: resume failed, starting {:016x} again", titleId);
			}
			else if (TitleIdParser(titleId).GetType() != TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE)
				CafeProcess::DiscardSuspendedTitle(); // another main app replaces it
		}
		CafeSystem::ShutdownTitleForSwitch(); // a suspended title stays (an applet opened from the HOME Menu)
		CafeSystem::PrepareForegroundTitle(titleId);
		CafeSystem::RequestRecreateCanvas();
		CafeSystem::LaunchForegroundTitle();
	}

	void OSShutdownThread(sint32 status)
	{
		CafeSystem::ShutdownTitle();
		CafeSystem::NotifyPPCProcessExit(status);
	}

	// Applets (00050030-xxxxxxxx) are launched on top of a "main app" (the Wii U Menu or a game) and return to it via
	// _SYSSwitchToMainApp. Cemu runs only one process, so the applet replaces the main app and we remember the
	// caller here (host memory, survives title switches) to switch back to it later
	static uint64 s_mainAppTitleId = 0;
	static uint64 s_callerTitleId = 0; // title that launched the current one (0 if started directly)
	static bool s_startedViaTitleSwitch = false;
	static std::atomic<uint64> s_queuedTitleSwitch = 0;

	static bool _IsAppletTitle(uint64 titleId)
	{
		return TitleIdParser(titleId).GetType() == TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE;
	}

	uint64 __OSGetMainAppTitleId()
	{
		return s_mainAppTitleId;
	}

	uint64 __OSGetCallerTitleId()
	{
		return s_callerTitleId;
	}

	void __OSQueueTitleSwitch(uint64 titleId)
	{
		s_queuedTitleSwitch.store(titleId);
	}

	static std::atomic<uint32> s_processGeneration = 0; // incremented on every title start
	static std::atomic<bool> s_forcedSwitchInProgress = false;

	// caller/main-app bookkeeping shared by all title switch paths
	static void _BeginTitleSwitch(uint64 titleId)
	{
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		if (!_IsAppletTitle(currentTitleId))
			s_mainAppTitleId = currentTitleId; // leaving a main app: it becomes the caller of whatever we launch
		s_callerTitleId = currentTitleId;
		s_startedViaTitleSwitch = true;
		s_queuedTitleSwitch.store(0);
		cemuLog_log(LogType::Force, "Title switch: {:016x} -> {:016x} (main app {:016x})", currentTitleId, titleId, s_mainAppTitleId);
	}

	// If the app doesn't exit within a few seconds after receiving the Exit message, perform the queued
	// title switch from the host side, like the _Exit path does. Saves are already done at this point
	// (the app signalled the foreground release / OSSavesDone_ReadyToRelease before Exit was sent)
	static void _StartExitWatchdog()
	{
		uint32 generation = s_processGeneration.load();
		std::thread([generation]() {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			if (generation != s_processGeneration.load() || !CafeSystem::IsTitleRunning())
				return; // process already exited/switched or emulation was stopped
			uint64 titleId = s_queuedTitleSwitch.exchange(0);
			if (titleId == 0)
				return; // the normal exit path claimed the switch
			s_forcedSwitchInProgress = true;
			cemuLog_log(LogType::Force, "System: app did not exit within 5s after Exit message, forcing queued switch to {:016x}", titleId);
			_BeginTitleSwitch(titleId);
			std::vector<std::string> noArgs;
			CafeSystem::SetOverrideArgs(noArgs);
			OSLauncherThread(titleId);
		}).detach();
	}

	uint32 __LaunchByTitleId(uint64 titleId, uint32 argc, MEMPTR<char>* argv)
	{
		_BeginTitleSwitch(titleId);
		// prepare argument buffer
		#if 0
		char argumentBuffer[4096];
		uint32 argumentBufferLength = 0;
		char* argWriter = argumentBuffer;
		for(uint32 i=0; i<argc; i++)
		{
			const char* arg = argv[i];
			uint32 argLength = strlen(arg);
			if((argumentBufferLength + argLength + 1) >= sizeof(argumentBuffer))
			{
				// argument buffer full
				cemuLog_logDebug(LogType::Force, "LaunchByTitleId: argument buffer full");
				return 0x80000000;
			}
			memcpy(argWriter, arg, argLength);
			argWriter[argLength] = '\0';
			argWriter += argLength + 1;
			argumentBufferLength += argLength + 1;
		}
		#endif
		// normally the above buffer is passed to the PPC kernel via syscall 0x2B and then
		// the kernel forwards it to IOSU MCP when requesting a title launch
		// but for now we HLE most of the launching code and can just set the argument array directly
		std::vector<std::string> argArray;
		for(uint32 i=0; i<argc; i++)
			argArray.emplace_back(argv[i]);
		CafeSystem::SetOverrideArgs(argArray);
		// spawn launcher thread (this current thread will be destroyed during relaunch)
		std::thread launcherThread(OSLauncherThread, titleId);
		launcherThread.detach();
		// suspend this thread
		coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
		return 0;
	}

	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc)
	{
		char appXmlPath[1024];
		cemu_assert_debug(argc == 0); // custom args not supported yet
		if(pathLength >= (sizeof(appXmlPath) - 32))
		{
			// path too long
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: path too long");
			return 0x80000000;
		}
		// read app.xml to get the titleId
		memcpy(appXmlPath, path, pathLength);
		appXmlPath[pathLength] = '\0';
		strcat(appXmlPath, "/code/app.xml");
		sint32 status;
		auto fscfile = fsc_open(appXmlPath, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
		if (!fscfile)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: failed to open target app.xml");
			return 0x80000000;
		}
		uint32 size = fsc_getFileSize(fscfile);
		std::vector<uint8> tmpData(size);
		fsc_readFile(fscfile, tmpData.data(), size);
		fsc_close(fscfile);
		// parse app.xml to get the titleId
		pugi::xml_document app_doc;
		if (!app_doc.load_buffer_inplace(tmpData.data(), tmpData.size()))
			return false;
		uint64 titleId = std::stoull(app_doc.child("app").child("title_id").child_value(), nullptr, 16);
		if(titleId == 0)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: failed to parse titleId from app.xml");
			return 0x80000000;
		}
		__LaunchByTitleId(titleId, 0, nullptr);
		return 0;
	}

	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv)
	{
		__LaunchByTitleId(CafeSystem::GetForegroundTitleId(), argc, argv);
		return 0;
	}

	// No power-off/reboot is ever pending. Title switches (SYSLaunchMenu, OSLaunchTitleByPathl, ...)
	// are handled by tearing down the process from a host thread, not via a shutdown request
	enum class OSShutdownReason : uint32
	{
		NoShutdown = 0,
	};

	OSShutdownReason OSGetShutdownReason()
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "OSGetShutdownReason() -> NoShutdown (first call, LR 0x{:08x})", PPCInterpreter_getCurrentInstance()->spr.LR);
			s_logged = true;
		}
		return OSShutdownReason::NoShutdown;
	}

	uint32 OSGetShutdownFlags()
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "OSGetShutdownFlags() -> 0 (first call, LR 0x{:08x})", PPCInterpreter_getCurrentInstance()->spr.LR);
			s_logged = true;
		}
		return 0;
	}

	bool s_transitionToBackground = false;
	bool s_transitionToForeground = false;

	// Process exit requested by the system side (e.g. after _SYSSwitchToMainApp):
	// 1) send ReleaseForeground with the shutdown flag set
	// 2) once the app has released the foreground (OSReleaseForeground via ProcUI, or OSSavesDone_ReadyToRelease) send Exit
	// The app then returns from main / calls exit() which performs any queued title switch
	static std::atomic<bool> s_exitRequested = false;
	static std::atomic<bool> s_releaseForShutdownPending = false;
	static std::atomic<bool> s_exitMessagePending = false;

	// set once the process has begun exiting (exit requested by the system, or exit/_Exit/__PPCExit called). Cleared
	// when the next process starts. Used by OSBlockThreadsOnExit, which only holds threads that exit during the exit
	static std::atomic<bool> s_processExiting = false;

	bool __OSIsProcessExiting()
	{
		return s_processExiting.load();
	}

	void __OSRequestProcessExit()
	{
		cemuLog_log(LogType::Force, "System: requesting process exit (ReleaseForeground -> Exit)");
		s_processExiting = true;
		s_exitRequested = true;
		s_releaseForShutdownPending = true;
	}

	static bool _SendSystemMessage(SysMessageId id, uint32 data1)
	{
		OSMessage msg{};
		msg.data0 = stdx::to_underlying(id);
		msg.data1 = data1;
		return OSSendMessage(coreinit::OSGetSystemMessageQueue(), &msg, 0);
	}

	/* HOME button (emulated controllers) */

	static bool s_homeButtonDown[16]{}; // per input source (VPAD and KPAD channels)
	static std::atomic<bool> s_homeCloseInProgress = false;

	static bool _IsWiiUMenuTitle(uint64 titleId)
	{
		return (titleId & 0xFFFFFFFFFFFFF0FFull) == 0x0005001010040000ull;
	}

	// HOME Menu applet 00050030-1001x10A (PFID 5)
	static bool _IsHomeMenuTitle(uint64 titleId)
	{
		return (titleId & 0xFFFFFFFFFFFFF0FFull) == 0x000500301001000Aull;
	}

	static constexpr sint32 SYS_PFID_HOME_MENU = 5;
	static std::atomic<bool> s_homeMenuLaunchPending = false;
	static uint32 s_homeMenuLaunchWaitReads = 0;

	static void _HandleHomeButtonPress(uint32 source)
	{
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		if (s_homeCloseInProgress || s_exitRequested || s_forcedSwitchInProgress)
		{
			cemuLog_log(LogType::Force, "HOME button: pressed while the title is already closing, ignored");
			return;
		}
		if (_IsHomeMenuTitle(currentTitleId))
			return; // the HOME Menu handles HOME itself (closes)
		if (s_homeMenuLaunchPending)
			return;
		// a game (any main app other than the Wii U Menu) gets the HOME Menu only if it can be suspended underneath it;
		// otherwise HOME closes it (below). A game that disabled HOME is told the press was denied
		bool openOverGame = !_IsWiiUMenuTitle(currentTitleId) && !_IsAppletTitle(currentTitleId) && CafeProcess::IsSuspendEnabled() && g_homeButtonMenuEnabled;
		if (_IsWiiUMenuTitle(currentTitleId) || openOverGame)
		{
			// open the HOME Menu over the Wii U Menu or a game. Capture the current frame first, the HOME Menu shows it behind itself
			cemuLog_log(LogType::Force, "HOME button: opening the HOME Menu over {:016x} ({})", currentTitleId, openOverGame ? "game" : "Wii U Menu");
			GX2::GX2_RequestLastFrameCapture();
			s_homeMenuLaunchWaitReads = 0;
			s_homeMenuLaunchPending = true;
			return;
		}
		if (!g_homeButtonMenuEnabled)
		{
			// the title disabled the HOME Menu (e.g. while saving), a console tells it the press was denied
			cemuLog_log(LogType::Force, "HOME button: HOME Menu disabled by the title, sending HomeButtonDenied");
			_SendSystemMessage(SysMessageId::HomeButtonDenied, 0);
			return;
		}
		uint64 targetTitleId;
		if (_IsAppletTitle(currentTitleId) && s_callerTitleId != 0 && s_mainAppTitleId != 0)
		{
			targetTitleId = s_mainAppTitleId;
			cemuLog_log(LogType::Force, "HOME button: closing applet {:016x}, returning to {:016x}", currentTitleId, targetTitleId);
		}
		else
		{
			targetTitleId = sysapp::GetWiiUMenuTitleId();
			if (targetTitleId == 0)
			{
				cemuLog_log(LogType::Force, "HOME button: no Wii U Menu installed, cannot close the software");
				return;
			}
			cemuLog_log(LogType::Force, "HOME button: \"Close software\" for {:016x}, returning to the Wii U Menu {:016x}", currentTitleId, targetTitleId);
		}
		s_homeCloseInProgress = true;
		// same path as closing from the HOME Menu: the title gets ReleaseForeground (shutdown) to save and release the
		// foreground, then Exit; its exit performs the queued switch
		__OSQueueTitleSwitch(targetTitleId);
		__OSRequestProcessExit();
		// deliver ReleaseForeground right away (the title may be blocked waiting on the system message queue)
		if (_SendSystemMessage(SysMessageId::MsgReleaseForeground, 1))
		{
			s_releaseForShutdownPending = false;
			cemuLog_log(LogType::Force, "HOME button: sent ReleaseForeground (shutdown), waiting for the title to save and release the foreground");
		}
	}

	// called from the input reads (VPADRead, KPADRead) with the HOME button state of a controller
	void __OSHomeButtonInput(uint32 source, bool isDown)
	{
		if (source >= std::size(s_homeButtonDown))
			return;
		if (s_homeMenuLaunchPending)
		{
			// wait until the frame capture is done (the renderer takes it on the next presented frame), at most ~30 input reads
			s_homeMenuLaunchWaitReads++;
			if (GX2::GX2_IsLastFrameCaptureDone() || s_homeMenuLaunchWaitReads > 30)
			{
				s_homeMenuLaunchPending = false;
				if (!GX2::GX2_IsLastFrameCaptureDone())
				{
					g_renderer->CancelScreenshotRequest();
					cemuLog_log(LogType::Force, "HOME button: last frame capture timed out, the HOME Menu background will be black");
				}
				if (CafeProcess::IsSuspendEnabled())
				{
					// keep the Wii U Menu or game alive, frozen, while the HOME Menu runs (resumed when the HOME Menu returns to it)
					uint64 homeMenuTitleId = sysapp::ResolvePFIDToTitleId(SYS_PFID_HOME_MENU);
					if (homeMenuTitleId == 0)
					{
						cemuLog_log(LogType::Force, "HOME button: the HOME Menu (00050030-1001x10A) is not installed");
						return;
					}
					cemuLog_log(LogType::Force, "HOME button: suspending {:016x} and opening the HOME Menu", CafeSystem::GetForegroundTitleId());
					_BeginTitleSwitch(homeMenuTitleId);
					std::thread([homeMenuTitleId]() {
						CafeProcess::SuspendForegroundTitleAndLaunch(homeMenuTitleId);
					}).detach();
					return;
				}
				if (sysapp::SwitchToPFID(SYS_PFID_HOME_MENU, "HOME button") != 0)
					cemuLog_log(LogType::Force, "HOME button: the HOME Menu (00050030-1001x10A) is not installed");
				return; // on success the switch suspends this thread
			}
		}
		bool wasDown = s_homeButtonDown[source];
		s_homeButtonDown[source] = isDown;
		if (isDown && !wasDown)
		{
			cemuLog_log(LogType::Force, "HOME button: pressed (input source {})", source);
			_HandleHomeButtonPress(source);
		}
	}

	// called once the app is done with its foreground release
	/* HOME Menu foreground handshake
	   On a console the HOME Menu runs in the background from boot: after initializing it asks the system to switch to
	   the app (OSSendAppSwitchRequest) and releases the foreground, and the system brings it to the foreground when HOME
	   is pressed. Cemu launches it on the HOME press instead, so the first release is answered with AcquireForeground
	   right away. A later switch request + release means the HOME Menu is done: Cemu runs one title at a time, so this
	   becomes a title switch back to the main app (the Wii U Menu) */
	enum class HomeMenuState : uint32 { None, Starting, StartupHandshake, Foreground, Returning };
	static std::atomic<HomeMenuState> s_homeMenuState = HomeMenuState::None;

	uint32 OSSendAppSwitchRequest(uint32 rampid, void* args, uint32 argsSize)
	{
		cemuLog_log(LogType::Force, "OSSendAppSwitchRequest(rampid {}, args 0x{:08x}, size 0x{:x}) (LR 0x{:08x})", rampid, MEMPTR<void>(args).GetMPTR(), argsSize, PPCInterpreter_getCurrentInstance()->spr.LR);
		HomeMenuState state = s_homeMenuState;
		if (state == HomeMenuState::Starting)
		{
			s_homeMenuState = HomeMenuState::StartupHandshake;
			cemuLog_log(LogType::Force, "HOME Menu: startup handshake, bringing it to the foreground once it released the foreground");
		}
		else if (state == HomeMenuState::Foreground)
		{
			s_homeMenuState = HomeMenuState::Returning;
			cemuLog_log(LogType::Force, "HOME Menu: switch request while in the foreground, returning to the main app once it released the foreground");
		}
		return 0;
	}

	// AcquireForeground for the HOME Menu is sent from an alarm shortly after the release: sending it directly from
	// OSReleaseForeground (still running on one of ProcUI's core threads while the others finish the release) made the
	// main thread start the acquire while the release was incomplete, and ProcUI's core threads deadlocked
	CafeStaticVar<OSAlarm_t> s_homeMenuForegroundAlarm{g_coreinitStaticData};

	static void _HomeMenuAcquireForegroundAlarmHandler(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::Force, "HOME Menu: sending AcquireForeground");
		OSMessage msg{};
		msg.data0 = stdx::to_underlying(SysMessageId::MsgAcquireForeground);
		msg.data1 = 1;
		msg.data2 = 1;
		if (!OSSendMessage(coreinit::OSGetSystemMessageQueue(), &msg, 0))
			s_transitionToForeground = true; // queue full, retry in UpdateSystemMessageQueue
		osLib_returnFromFunction(hCPU, 0);
	}

	// returns true if the release was part of the HOME Menu handshake and has been handled. Only the final release
	// (OSReleaseForeground, called by ProcUI's core threads once the release is complete) counts: answering the earlier
	// OSSavesDone_ReadyToRelease (called from within ProcUIProcessMessages, before ProcUI started its background thread)
	// delivered AcquireForeground in the middle of the release and left ProcUI stuck
	static bool _OnHomeMenuForegroundReleased(const char* source)
	{
		if (strcmp(source, "OSReleaseForeground") != 0)
		{
			HomeMenuState state = s_homeMenuState;
			return state == HomeMenuState::StartupHandshake || state == HomeMenuState::Returning; // wait for OSReleaseForeground
		}
		HomeMenuState expected = HomeMenuState::StartupHandshake;
		if (s_homeMenuState.compare_exchange_strong(expected, HomeMenuState::Foreground))
		{
			cemuLog_log(LogType::Force, "HOME Menu: {} during startup, bringing it to the foreground in 100 ms", source);
			OSCreateAlarm(s_homeMenuForegroundAlarm.GetPtr());
			OSSetAlarm(s_homeMenuForegroundAlarm.GetPtr(), ESPRESSO_TIMER_CLOCK / 10, RPLLoader_MakePPCCallable(_HomeMenuAcquireForegroundAlarmHandler));
			return true;
		}
		expected = HomeMenuState::Returning;
		if (s_homeMenuState.compare_exchange_strong(expected, HomeMenuState::None))
		{
			uint64 targetTitleId = s_mainAppTitleId != 0 ? s_mainAppTitleId : sysapp::GetWiiUMenuTitleId();
			cemuLog_log(LogType::Force, "HOME Menu: {}, returning to {:016x}", source, targetTitleId);
			if (targetTitleId != 0)
				__LaunchByTitleId(targetTitleId, 0, nullptr); // suspends this thread
			return true;
		}
		return false;
	}

	static void _OnForegroundReleased(const char* source)
	{
		if (_OnHomeMenuForegroundReleased(source))
			return;
		if (!s_exitRequested.exchange(false)) // send Exit only once even if both release paths fire
			return;
		cemuLog_log(LogType::Force, "System: {} after exit request, sending Exit message", source);
		s_releaseForShutdownPending = false;
		if (!_SendSystemMessage(SysMessageId::MsgExit, 0))
			s_exitMessagePending = true; // queue full, retry in UpdateSystemMessageQueue
		_StartExitWatchdog();
	}

	void OSReleaseForeground()
	{
		cemuLog_logDebug(LogType::Force, "OSReleaseForeground");
		_OnForegroundReleased("OSReleaseForeground");
	}

	void OSSavesDone_ReadyToRelease()
	{
		cemuLog_log(LogType::Force, "OSSavesDone_ReadyToRelease()");
		_OnForegroundReleased("OSSavesDone_ReadyToRelease");
	}

	void StartBackgroundForegroundTransition()
	{
		s_transitionToBackground = true;
		s_transitionToForeground = true;
	}

	// called at the beginning of OSReceiveMessage if the queue is the system message queue
	void UpdateSystemMessageQueue()
	{
		if(!OSIsInterruptEnabled())
			return;
		cemu_assert_debug(!__OSHasSchedulerLock());
		// normally syscall 0x2E is used to get the next message
		// for now we just have some preliminary logic here to allow a fake transition to background & foreground
		if (s_exitMessagePending)
		{
			if (_SendSystemMessage(SysMessageId::MsgExit, 0))
				s_exitMessagePending = false;
			return;
		}
		if (s_releaseForShutdownPending)
		{
			if (_SendSystemMessage(SysMessageId::MsgReleaseForeground, 1)) // 1 -> system is shutting down
				s_releaseForShutdownPending = false;
			return;
		}
		if(s_transitionToBackground)
		{
			// add transition to background message
			OSMessage msg{};
			msg.data0 = stdx::to_underlying(SysMessageId::MsgReleaseForeground);
			msg.data1 = 0; // 1 -> System is shutting down 0 -> Begin transitioning to background
			OSMessageQueue* systemMessageQueue = coreinit::OSGetSystemMessageQueue();
			if(OSSendMessage(systemMessageQueue, &msg, 0))
				s_transitionToBackground = false;
			return;
		}
		if(s_transitionToForeground)
		{
			// add transition to foreground message
			OSMessage msg{};
			msg.data0 = stdx::to_underlying(SysMessageId::MsgAcquireForeground);
			msg.data1 = 1; // ?
			msg.data2 = 1; // ?
			OSMessageQueue* systemMessageQueue = coreinit::OSGetSystemMessageQueue();
			if(OSSendMessage(systemMessageQueue, &msg, 0))
				s_transitionToForeground = false;
			return;
		}
	}

	// called when OSReceiveMessage returns a message from the system message queue
	void HandleReceivedSystemMessage(OSMessage* msg)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		cemuLog_log(LogType::Force, "Receiving message: {:08x}", (uint32)msg->data0);
	}

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Register stubbed");
		return 0;
	}

	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Deregister stubbed");
		return 0;
	}

	void __PPCExit(sint32 status)
	{
		// spawn shutdown thread (the current thread has to be destroyed as part of the shutdown process)
		std::thread shutdownThread(OSShutdownThread, status);
		shutdownThread.detach();
		// suspend this thread
		coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
	}

	// ends the process. If a title switch was queued (e.g. by _SYSSwitchToMainApp) the queued title is launched instead
	static void __ExitProcessOrSwitch(sint32 status)
	{
		s_processExiting = true;
		if (s_forcedSwitchInProgress)
		{
			// the exit watchdog is already tearing down this process to switch titles, don't end emulation
			cemuLog_log(LogType::Force, "Process exit with status {} during forced title switch, suspending thread", status);
			coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
			return;
		}
		uint64 queuedTitleId = s_queuedTitleSwitch.exchange(0);
		if (queuedTitleId != 0)
		{
			cemuLog_log(LogType::Force, "Process exit with status {}: performing queued switch to {:016x}", status, queuedTitleId);
			__LaunchByTitleId(queuedTitleId, 0, nullptr);
			return;
		}
		__PPCExit(status);
	}

	void coreinit_exit(sint32 status)
	{
		cemuLog_log(LogType::Force, "The title terminated the process by calling coreinit.exit({})", (sint32)status);
		DebugLogStackTrace(coreinit::OSGetCurrentThread(), coreinit::OSGetStackPointer());
		if (gCoreinitData->__atexit_cleanup)
			PPCCoreCallback(gCoreinitData->__atexit_cleanup, status);
		if (gCoreinitData->__stdio_cleanup)
			PPCCoreCallback(gCoreinitData->__stdio_cleanup);
		__ExitProcessOrSwitch(status);
	}

	// like exit() but without running atexit/stdio cleanup. Used by applets after _SYSSwitchToMainApp
	void coreinit__Exit(sint32 status)
	{
		cemuLog_log(LogType::Force, "The title terminated the process by calling coreinit._Exit({})", (sint32)status);
		__ExitProcessOrSwitch(status);
	}

	// __PPCExit called by the title itself (some titles end the process with it instead of exit/_Exit, e.g. New Super
	// Mario Bros. U after its Exit message). Like exit/_Exit it performs a queued title switch instead of ending emulation
	void coreinit___PPCExit(sint32 status)
	{
		cemuLog_log(LogType::Force, "The title terminated the process by calling coreinit.__PPCExit({})", (sint32)status);
		__ExitProcessOrSwitch(status);
	}

	void miscInit()
	{
		s_currentTitleId = CafeSystem::GetForegroundTitleId();
		// title started directly (game list, command line) rather than by a guest title switch: reset caller tracking
		if (!s_startedViaTitleSwitch)
		{
			s_mainAppTitleId = _IsAppletTitle(s_currentTitleId) ? 0 : s_currentTitleId;
			s_callerTitleId = 0;
		}
		s_startedViaTitleSwitch = false;
		s_queuedTitleSwitch.store(0);
		s_processGeneration++;
		s_forcedSwitchInProgress = false;
		s_sdkVersion = CafeSystem::GetForegroundTitleSDKVersion();
		s_forwardConsoleLogs = LaunchSettings::ForwardConsoleLogging();
		s_transitionToBackground = false;
		s_transitionToForeground = false;
		s_exitRequested = false;
		s_processExiting = false;
		s_releaseForShutdownPending = false;
		s_exitMessagePending = false;
		s_homeCloseInProgress = false;
		s_homeMenuLaunchPending = false;
		s_homeMenuState = _IsHomeMenuTitle(s_currentTitleId) ? HomeMenuState::Starting : HomeMenuState::None;
		for (auto& it : s_homeButtonDown)
			it = false;
		// the HOME Menu is available unless the title disables it (OSEnableHomeButtonMenu)
		g_homeButtonMenuEnabled = true;

		cafeExportRegister("coreinit", __os_snprintf, LogType::Placeholder);

		cafeExportRegister("coreinit", COSVReport, LogType::Placeholder);
		cafeExportRegister("coreinit", COSWarn, LogType::Placeholder);
		cafeExportRegister("coreinit", OSReport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSVReport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSLogPrintf, LogType::Placeholder);
		cafeExportRegister("coreinit", OSConsoleWrite, LogType::Placeholder);
		cafeExportRegister("coreinit", OSFatal, LogType::Placeholder);

		cafeExportRegister("coreinit", OSGetPFID, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetUPID, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetTitleID, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSGetProcessSDKVersion, LogType::Placeholder);

		g_homeButtonMenuEnabled = true; // enabled by default
		// Disney Infinity 2.0 actually relies on home button menu being enabled by default. If it's false it will crash due to calling erreula->IsAppearHomeNixSign() before initializing erreula
		cafeExportRegister("coreinit", OSIsHomeButtonMenuEnabled, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendAppSwitchRequest, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableHomeButtonMenu, LogType::CoreinitThread);

		cafeExportRegister("coreinit", OSLaunchTitleByPathl, LogType::Placeholder);
		cafeExportRegister("coreinit", OSRestartGame, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetShutdownReason, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetShutdownFlags, LogType::Placeholder);

		cafeExportRegister("coreinit", OSReleaseForeground, LogType::Placeholder);
		cafeExportRegister("coreinit", OSSavesDone_ReadyToRelease, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDriver_Register, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDriver_Deregister, LogType::Placeholder);

		cafeExportRegisterFunc(coreinit_exit, "coreinit", "exit", LogType::CoreinitThread);
		cafeExportRegisterFunc(coreinit__Exit, "coreinit", "_Exit", LogType::CoreinitThread);
		cafeExportRegisterFunc(coreinit___PPCExit, "coreinit", "__PPCExit", LogType::CoreinitThread);
	}

};

// per-process state, saved/restored when the title is suspended (see Cafe/CafeProcessState.h)
namespace coreinit
{
	CAFE_PROCESS_STATE(s_currentTitleId);
	CAFE_PROCESS_STATE(s_sdkVersion);
	CAFE_PROCESS_STATE(g_homeButtonMenuEnabled);
	CAFE_PROCESS_STATE(s_transitionToBackground);
	CAFE_PROCESS_STATE(s_transitionToForeground);
	CAFE_PROCESS_STATE(s_exitRequested);
	CAFE_PROCESS_STATE(s_processExiting);
	CAFE_PROCESS_STATE(s_releaseForShutdownPending);
	CAFE_PROCESS_STATE(s_exitMessagePending);
	CAFE_PROCESS_STATE(s_homeButtonDown);
	CAFE_PROCESS_STATE(s_homeCloseInProgress);
	CAFE_PROCESS_STATE(s_homeMenuLaunchPending);
	CAFE_PROCESS_STATE(s_homeMenuLaunchWaitReads);
	CAFE_PROCESS_STATE(s_homeMenuState);
}
