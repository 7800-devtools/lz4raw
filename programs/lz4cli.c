/*
  LZ4cli - LZ4 Command Line Interface
  Copyright (C) Yann Collet 2011-2020

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user program of the LZ4 library.
  The license of LZ4 library is BSD.
  The license of xxHash library is BSD.
  The license of this compression CLI program is GPLv2.
*/


/****************************
*  Includes
*****************************/
#include "platform.h" /* Compiler options, IS_CONSOLE */
#include "util.h"     /* UTIL_HAS_CREATEFILELIST, UTIL_createFileList */
#include <stdio.h>    /* fprintf, getchar */
#include <stdlib.h>   /* exit, calloc, free */
#include <string.h>   /* strcmp, strlen */
#include "bench.h"    /* BMK_benchFile, BMK_SetNbIterations, BMK_SetBlocksize, BMK_SetPause */
#include "lz4io.h"    /* LZ4IO_compressFilename, LZ4IO_decompressFilename, LZ4IO_compressMultipleFilenames */
#include "lz4hc.h"    /* LZ4HC_CLEVEL_MAX */
#include "lz4.h"      /* LZ4_VERSION_STRING */


/*****************************
*  Constants
******************************/
#define COMPRESSOR_NAME "LZ4 command line interface"
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %i-bits v%s, by %s ***\n", COMPRESSOR_NAME, (int)(sizeof(void*)*8), LZ4_versionString(), AUTHOR
#define LZ4_EXTENSION ".lz4"
#define LZ4CAT "lz4cat"
#define UNLZ4 "unlz4"
#define LZ4_LEGACY "lz4c"
static int g_lz4c_legacy_commands = 0;

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define LZ4_BLOCKSIZEID_DEFAULT 7


/*-************************************
*  Macros
***************************************/
#define DISPLAYOUT(...)        fprintf(stdout, __VA_ARGS__)
#define DISPLAY(...)           fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)   if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static unsigned displayLevel = 2;   /* 0 : no display ; 1: errors only ; 2 : downgradable normal ; 3 : non-downgradable normal; 4 : + information */


/*-************************************
*  Exceptions
***************************************/
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/*-************************************
*  Version modifiers
***************************************/
#define DEFAULT_COMPRESSOR   LZ4IO_compressFilename
#define DEFAULT_DECOMPRESSOR LZ4IO_decompressFilename
int LZ4IO_compressFilename_Legacy(const char* input_filename, const char* output_filename, int compressionlevel, const LZ4IO_prefs_t* prefs);   /* hidden function */
int LZ4IO_compressMultipleFilenames_Legacy(
                            const char** inFileNamesTable, int ifntSize,
                            const char* suffix,
                            int compressionLevel, const LZ4IO_prefs_t* prefs);

/*-***************************
*  Functions
*****************************/
static int usage(const char* exeName)
{
    DISPLAY( "Usage : \n");
    DISPLAY( "      %s [input] [output] \n", exeName);
    return 0;
}

static int badusage(const char* exeName)
{
    DISPLAYLEVEL(1, "Incorrect parameters\n");
    if (displayLevel >= 1) usage(exeName);
    exit(1);
}


static void waitEnter(void)
{
    DISPLAY("Press enter to continue...\n");
    (void)getchar();
}

static const char* lastNameFromPath(const char* path)
{
    const char* name = path;
    if (strrchr(name, '/')) name = strrchr(name, '/') + 1;
    if (strrchr(name, '\\')) name = strrchr(name, '\\') + 1; /* windows */
    return name;
}

/*! exeNameMatch() :
    @return : a non-zero value if exeName matches test, excluding the extension
   */
static int exeNameMatch(const char* exeName, const char* test)
{
    return !strncmp(exeName, test, strlen(test)) &&
        (exeName[strlen(test)] == '\0' || exeName[strlen(test)] == '.');
}

/*! readU32FromChar() :
 * @return : unsigned integer value read from input in `char` format
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function result can overflow if digit string > MAX_UINT */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        result *= 10;
        result += (unsigned)(**stringPtr - '0');
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        result <<= 10;
        if (**stringPtr=='M') result <<= 10;
        (*stringPtr)++ ;
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

/** longCommandWArg() :
 *  check if *stringPtr is the same as longCommand.
 *  If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 * @return 0 and doesn't modify *stringPtr otherwise.
 */
static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}

typedef enum { om_auto, om_compress, om_decompress, om_test, om_bench, om_list } operationMode_e;

/** determineOpMode() :
 *  auto-determine operation mode, based on input filename extension
 *  @return `om_decompress` if input filename has .lz4 extension and `om_compress` otherwise.
 */
static operationMode_e determineOpMode(const char* inputFilename)
{
    size_t const inSize  = strlen(inputFilename);
    size_t const extSize = strlen(LZ4_EXTENSION);
    size_t const extStart= (inSize > extSize) ? inSize-extSize : 0;
    if (!strcmp(inputFilename+extStart, LZ4_EXTENSION)) return om_decompress;
    else return om_compress;
}

int main(int argc, const char** argv)
{
    int i,
        cLevel=1,
        cLevelLast=-10000,
        legacy_format=0,
        forceStdout=0,
        main_pause=0,
        multiple_inputs=0,
        all_arguments_are_files=0,
        operationResult=0;
    operationMode_e mode = om_auto;
    const char* input_filename = NULL;
    const char* output_filename= NULL;
    const char* dictionary_filename = NULL;
    char* dynNameSpace = NULL;
    const char** inFileNames = (const char**)calloc((size_t)argc, sizeof(char*));
    unsigned ifnIdx=0;
    LZ4IO_prefs_t* const prefs = LZ4IO_defaultPreferences();
    const char nullOutput[] = NULL_OUTPUT;
    const char extension[] = LZ4_EXTENSION;
    size_t blockSize = LZ4IO_setBlockSizeID(prefs, LZ4_BLOCKSIZEID_DEFAULT);
    const char* const exeName = lastNameFromPath(argv[0]);
#ifdef UTIL_HAS_CREATEFILELIST
    const char** extendedFileList = NULL;
    char* fileNamesBuf = NULL;
    unsigned fileNamesNb, recursive=0;
#endif

    /* Init */
    if (inFileNames==NULL) {
        DISPLAY("Allocation error : not enough memory \n");
        return 1;
    }
    inFileNames[0] = stdinmark;
    LZ4IO_setOverwrite(prefs, 0);

    // lz4raw - No CRC32 frame checksums, max compression, legacy mode
    LZ4IO_setStreamChecksumMode(prefs, 0);
    cLevel = 9;
    legacy_format = 1;

    /* predefined behaviors, based on binary/link name */
    if (exeNameMatch(exeName, LZ4CAT)) {
        mode = om_decompress;
        LZ4IO_setOverwrite(prefs, 1);
        LZ4IO_setPassThrough(prefs, 1);
        LZ4IO_setRemoveSrcFile(prefs, 0);
        forceStdout=1;
        output_filename=stdoutmark;
        displayLevel=1;
        multiple_inputs=1;
    }
    if (exeNameMatch(exeName, UNLZ4)) { mode = om_decompress; }
    if (exeNameMatch(exeName, LZ4_LEGACY)) { g_lz4c_legacy_commands=1; }

    /* command switches */
    for(i=1; i<argc; i++) {
        const char* argument = argv[i];

        if(!argument) continue;   /* Protection if argument empty */

        /* Short commands (note : aggregated short commands are allowed) */
        if (!all_arguments_are_files && argument[0]=='-') {
            /* '-' means stdin/stdout */
            if (argument[1]==0) {
                if (!input_filename) input_filename=stdinmark;
                else output_filename=stdoutmark;
                continue;
            }

            /* long commands (--long-word) */
            if (argument[1]=='-') {
                if (!strcmp(argument,  "--help")) { usage(exeName); goto _cleanup; }
                    }

            while (argument[1]!=0) {
                argument ++;

                switch(argument[0])
                {
                    /* Display help */
                case 'V': DISPLAYOUT(WELCOME_MESSAGE); goto _cleanup;   /* Version */
                case 'h': usage(exeName); goto _cleanup;
                case 'H': usage(exeName); goto _cleanup;
                    /* Unrecognised command */
                default : usage(exeName);
                }
            }
            continue;
        }

        /* Store in *inFileNames[] if -m is used. */
        if (multiple_inputs) { inFileNames[ifnIdx++]=argument; continue; }

        /* Store first non-option arg in input_filename to preserve original cli logic. */
        if (!input_filename) { input_filename=argument; continue; }

        /* Second non-option arg in output_filename to preserve original cli logic. */
        if (!output_filename) {
            output_filename=argument;
            if (!strcmp (output_filename, nullOutput)) output_filename = nulmark;
            continue;
        }

        /* 3rd non-option arg should not exist */
        DISPLAYLEVEL(1, "Warning : %s won't be used ! Do you want multiple input files (-m) ? \n", argument);
    }

    DISPLAYLEVEL(3, WELCOME_MESSAGE);
#ifdef _POSIX_C_SOURCE
    DISPLAYLEVEL(4, "_POSIX_C_SOURCE defined: %ldL\n", (long) _POSIX_C_SOURCE);
#endif
#ifdef _POSIX_VERSION
    DISPLAYLEVEL(4, "_POSIX_VERSION defined: %ldL\n", (long) _POSIX_VERSION);
#endif
#ifdef PLATFORM_POSIX_VERSION
    DISPLAYLEVEL(4, "PLATFORM_POSIX_VERSION defined: %ldL\n", (long) PLATFORM_POSIX_VERSION);
#endif
#ifdef _FILE_OFFSET_BITS
    DISPLAYLEVEL(4, "_FILE_OFFSET_BITS defined: %ldL\n", (long) _FILE_OFFSET_BITS);
#endif
    if ((mode == om_compress) || (mode == om_bench))
        DISPLAYLEVEL(4, "Blocks size : %u KB\n", (U32)(blockSize>>10));

    if (multiple_inputs) {
        input_filename = inFileNames[0];
#ifdef UTIL_HAS_CREATEFILELIST
        if (recursive) {  /* at this stage, filenameTable is a list of paths, which can contain both files and directories */
            extendedFileList = UTIL_createFileList(inFileNames, ifnIdx, &fileNamesBuf, &fileNamesNb);
            if (extendedFileList) {
                unsigned u;
                for (u=0; u<fileNamesNb; u++) DISPLAYLEVEL(4, "%u %s\n", u, extendedFileList[u]);
                free((void*)inFileNames);
                inFileNames = extendedFileList;
                ifnIdx = fileNamesNb;
        }   }
#endif
    }

    if (dictionary_filename) {
        if (!strcmp(dictionary_filename, stdinmark) && IS_CONSOLE(stdin)) {
            usage(exeName);
            //DISPLAYLEVEL(1, "refusing to read from a console\n");
            exit(1);
        }
        LZ4IO_setDictionaryFilename(prefs, dictionary_filename);
    }

    /* benchmark and test modes */
    if (mode == om_bench) {
        BMK_setNotificationLevel(displayLevel);
        operationResult = BMK_benchFiles(inFileNames, ifnIdx, cLevel, cLevelLast, dictionary_filename);
        goto _cleanup;
    }

    if (mode == om_test) {
        LZ4IO_setTestMode(prefs, 1);
        output_filename = nulmark;
        mode = om_decompress;   /* defer to decompress */
    }

    /* compress or decompress */
    if (!input_filename) input_filename = stdinmark;
    /* Check if input is defined as console; trigger an error in this case */
    if (!strcmp(input_filename, stdinmark) && IS_CONSOLE(stdin) ) {
        usage(exeName);
        //DISPLAYLEVEL(1, "refusing to read from a console\n");
        exit(1);
    }
    if (!strcmp(input_filename, stdinmark)) {
        /* if input==stdin and no output defined, stdout becomes default output */
        if (!output_filename) output_filename = stdoutmark;
    }
    else{
#ifdef UTIL_HAS_CREATEFILELIST
        if (!recursive && !UTIL_isRegFile(input_filename)) {
#else
        if (!UTIL_isRegFile(input_filename)) {
#endif
            DISPLAYLEVEL(1, "%s: is not a regular file \n", input_filename);
            exit(1);
        }
    }

    /* No output filename ==> try to select one automatically (when possible) */
    while ((!output_filename) && (multiple_inputs==0)) {
        if (!IS_CONSOLE(stdout) && mode != om_list) {
            /* Default to stdout whenever stdout is not the console.
             * Note : this policy may change in the future, therefore don't rely on it !
             * To ensure `stdout` is explicitly selected, use `-c` command flag.
             * Conversely, to ensure output will not become `stdout`, use `-m` command flag */
            DISPLAYLEVEL(1, "Warning : using stdout as default output. Do not rely on this behavior: use explicit `-c` instead ! \n");
            output_filename=stdoutmark;
            break;
        }
        if (mode == om_auto) {  /* auto-determine compression or decompression, based on file extension */
            mode = determineOpMode(input_filename);
        }
        if (mode == om_compress) {   /* compression to file */
            size_t const l = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,l+5);
            if (dynNameSpace==NULL) { perror(exeName); exit(1); }
            strcpy(dynNameSpace, input_filename);
            strcat(dynNameSpace, LZ4_EXTENSION);
            output_filename = dynNameSpace;
            DISPLAYLEVEL(2, "Compressed filename will be : %s \n", output_filename);
            break;
        }
        if (mode == om_decompress) {/* decompression to file (automatic name will work only if input filename has correct format extension) */
            size_t outl;
            size_t const inl = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,inl+1);
            if (dynNameSpace==NULL) { perror(exeName); exit(1); }
            strcpy(dynNameSpace, input_filename);
            outl = inl;
            if (inl>4)
                while ((outl >= inl-4) && (input_filename[outl] ==  extension[outl-inl+4])) dynNameSpace[outl--]=0;
            if (outl != inl-5) { DISPLAYLEVEL(1, "Cannot determine an output filename\n"); badusage(exeName); }
            output_filename = dynNameSpace;
            DISPLAYLEVEL(2, "Decoding file %s \n", output_filename);
        }
        break;
    }

    if (mode == om_list){
        if(!multiple_inputs){
            inFileNames[ifnIdx++] = input_filename;
        }
    }
    else{
        if (multiple_inputs==0) assert(output_filename);
    }
    /* when multiple_inputs==1, output_filename may simply be useless,
     * however, output_filename must be !NULL for next strcmp() tests */
    if (!output_filename) output_filename = "*\\dummy^!//";

    /* Check if output is defined as console; trigger an error in this case */
    if ( !strcmp(output_filename,stdoutmark)
      && mode != om_list
      && IS_CONSOLE(stdout)
      && !forceStdout) {
        DISPLAYLEVEL(1, "refusing to write to console without -c \n");
        exit(1);
    }
    /* Downgrade notification level in stdout and multiple file mode */
    if (!strcmp(output_filename,stdoutmark) && (displayLevel==2)) displayLevel=1;
    if ((multiple_inputs) && (displayLevel==2)) displayLevel=1;

    /* Auto-determine compression or decompression, based on file extension */
    if (mode == om_auto) {
        mode = determineOpMode(input_filename);
    }

    /* IO Stream/File */
    LZ4IO_setNotificationLevel((int)displayLevel);
    if (ifnIdx == 0) multiple_inputs = 0;
    if (mode == om_decompress) {
        if (multiple_inputs) {
            const char* const dec_extension = !strcmp(output_filename,stdoutmark) ? stdoutmark : LZ4_EXTENSION;
            assert(ifnIdx <= INT_MAX);
            operationResult = LZ4IO_decompressMultipleFilenames(inFileNames, (int)ifnIdx, dec_extension, prefs);
        } else {
            operationResult = DEFAULT_DECOMPRESSOR(input_filename, output_filename, prefs);
        }
    } else if (mode == om_list){
        operationResult = LZ4IO_displayCompressedFilesInfo(inFileNames, ifnIdx);
    } else {   /* compression is default action */
        if (legacy_format) {
            DISPLAYLEVEL(3, "! Generating LZ4 Legacy format (deprecated) ! \n");
            if(multiple_inputs){
                const char* const leg_extension = !strcmp(output_filename,stdoutmark) ? stdoutmark : LZ4_EXTENSION;
                LZ4IO_compressMultipleFilenames_Legacy(inFileNames, (int)ifnIdx, leg_extension, cLevel, prefs);
            } else {
                LZ4IO_compressFilename_Legacy(input_filename, output_filename, cLevel, prefs);
            }
        } else {
            if (multiple_inputs) {
                const char* const comp_extension = !strcmp(output_filename,stdoutmark) ? stdoutmark : LZ4_EXTENSION;
                assert(ifnIdx <= INT_MAX);
                operationResult = LZ4IO_compressMultipleFilenames(inFileNames, (int)ifnIdx, comp_extension, cLevel, prefs);
            } else {
                operationResult = DEFAULT_COMPRESSOR(input_filename, output_filename, cLevel, prefs);
    }   }   }

_cleanup:
    if (main_pause) waitEnter();
    free(dynNameSpace);
#ifdef UTIL_HAS_CREATEFILELIST
    if (extendedFileList) {
        UTIL_freeFileList(extendedFileList, fileNamesBuf);
        inFileNames = NULL;
    }
#endif
    LZ4IO_freePreferences(prefs);
    free((void*)inFileNames);
    return operationResult;
}
