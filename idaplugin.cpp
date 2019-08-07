/**
 * @file idaplugin/idaplugin.cpp
 * @brief Plugin entry point - definition of plugin's intarface.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

 //Build Dependencies:
 //IDA Pro 7.2 SDK
 //RetDec v3.3: https://github.com/avast/retdec config and utils libraries
 //RetDec IDA Plugin v0.9: https://github.com/avast/retdec-idaplugin
 //jsoncpp library v1.8.4: https://github.com/open-source-parsers/jsoncpp
 //whereami library: https://github.com/gpakosz/whereami

 //Runtime Dependencies: Ghidra 9.0.4 path specified (JDK v11+ not necessary for decompile.exe/sleigh.exe in Ghidra/Features/Decompiler/os/*/decompile[.exe], sleigh modules in Ghidra/Processors)
 //Plugin copied to IDA Pro 7.2 plugins folder
//conversion.h in x86 needs a fix for adding __cdecl to the ios_base return pointer format as in std::ios_base &(__cdecl* format)(std::ios_base &) = std::dec

#include <fstream>
#include <iostream>

#if !defined(OS_WINDOWS) // Linux || macOS
	#include <signal.h>
#endif

#include "retdec/utils/file_io.h"
#include "retdec/utils/filesystem_path.h"
#include "code_viewer.h"
#include "config_generator.h"
#include "decompiler.h"
#include "defs.h"
#include "plugin_config.h"
#include "sleighinterface.h"

namespace idaplugin {

/**
 * General info used by plugin.
 */
RdGlobalInfo* decompInfo;

/**
 * Kill old thread if still running.
 */
void killDecompilation(bool bTerminate)
{
	if (decompInfo->decompRunning)
	{
		INFO_MSG("Unfinished decompilation was KILLED !!! Only one decompilation can run at a time.\n");
		{ //must be a locked unit otherwise race condition can occur
			qmutex_locker_t lock(decompInfo->qm);
			decompInfo->exiting = true;			
			if (decompInfo->uiExecutingTask != -1) {
				if (cancel_exec_request(decompInfo->uiExecutingTask)) {
					qsem_post(decompInfo->termSem);
				}
			}
		}
		//obviously killing a thread is a last case resort which risks a large amount of side effects
		stopDecompilation(decompInfo, false, false, false); //first kill only the decompilation process
		//qsleep(3000); //wait 3 seconds since no way to poll thread status
		//qthread_kill(decompInfo->decompThread);
		qthread_join(decompInfo->decompThread);
		qthread_free(decompInfo->decompThread);
		decompInfo->decompThread = nullptr;

		stopDecompilation(decompInfo, false, false, true);
		decompInfo->uiExecutingTask = -1;
		decompInfo->exiting = false;
		/*
		if (decompInfo->decompPid)
		{
			//term_process()...
#if defined(OS_WINDOWS) //OpenProcess/WaitForSingleObject/KillProcess...or better yet keep the process handle, the only clean way in Windows
			std::string cmd = "taskkill /F /T /PID " + std::to_string(decompInfo->decompPid);
			std::system(cmd.c_str());
#else // Linux || macOS
			kill(decompInfo->decompPid, SIGTERM);
#endif
			decompInfo->decompPid = 0;
		}
		if (decompInfo->rdHandle != -1) {
			_close(decompInfo->rdHandle); decompInfo->rdHandle = -1;
		}
		if (decompInfo->wrHandle != -1) {
			_close(decompInfo->wrHandle); decompInfo->wrHandle = -1;
		}*/

		decompInfo->decompRunning = false;
	} else {
		if (decompInfo->decompThread != nullptr) {
			qthread_join(decompInfo->decompThread);
			qthread_free(decompInfo->decompThread);
			decompInfo->decompThread = nullptr;
		}
		if (bTerminate) stopDecompilation(decompInfo, true, false, true);
		else stopDecompilation(decompInfo, false, true, true); //make sure did not exit before starting again
	}
}

/**
 * Save IDA database before decompilation to protect it if something goes wrong.
 * @param inSitu If true, DB is saved with the same name as IDA default database.
 * @param suffix If @p inSitu is false, use this suffix to distinguish DBs.
 */
void saveIdaDatabase(bool inSitu = false, const std::string &suffix = ".dec-backup")
{
	INFO_MSG("Saving IDA database ...\n");

	std::string workIdb = decompInfo->workIdb;

	auto dotPos = workIdb.find_last_of(".");
	if (dotPos != std::string::npos)
	{
		workIdb.erase(dotPos, std::string::npos);
	}

	if (!inSitu)
	{
		workIdb += suffix;
	}

	workIdb += std::string(".") + IDB_EXT;

	save_database(workIdb.c_str(), DBFL_COMP);

	INFO_MSG("IDA database saved into :  " << workIdb << "\n");
}

/**
 * Generate retargetable decompiler database from IDA database.
 */
void generatePluginDatabase()
{
	INFO_MSG("Generating retargetable decompilation DB ...\n");

	ConfigGenerator jg(*decompInfo);
	decompInfo->dbFile = jg.generate();
}

/**
 * Find out if input file is relocatable -- object file.
 * @return @c True if file relocatable, @c false otherwise.
 */
bool isRelocatable()
{
#ifdef __X64__
	if (inf.filetype == f_COFF && inf.start_ea == BADADDR)
#else
	if (inf.filetype == f_COFF && inf.beginEA == BADADDR)
#endif
	{
		return true;
	}
	else if (inf.filetype == f_ELF)
	{
		std::ifstream infile(decompInfo->inputPath, std::ios::binary);
		if (infile.good())
		{
			std::size_t e_type_offset = 0x10;
			infile.seekg(e_type_offset, std::ios::beg);

			// relocatable -- constant 0x1 at <0x10-0x11>
			// little endian -- 0x01 0x00
			// big endian -- 0x00 0x01
			char b1 = 0;
			char b2 = 0;
			if (infile.get(b1))
			{
				if (infile.get(b2))
				{
					if (std::size_t(b1) + std::size_t(b2) == 1)
					{
						return true;
					}
				}
			}
		}
	}

	// f_BIN || f_PE || f_HEX || other
	return false;
}

/**
 * Decompile only provided function or if nothing provided then the current function under focus.
 * @param fnc2decomp Function to decompile.
 * @param force      If @c true, decompilation is always performed.
 */
void runSelectiveDecompilation(func_t *fnc2decomp = nullptr, bool force = false)
{
	if (decompInfo->decompThread != nullptr) {
		if (!decompInfo->decompRunning) killDecompilation(false);
		else if (ask_buttons("Yes", "No", nullptr, ASKBTN_YES, "A prior decompilation is still running, would you like to terminate it to process the new request?") == ASKBTN_YES) {
			killDecompilation(true);
		} else return;
	}
	if (isRelocatable() && inf.min_ea != 0)
	{
		WARNING_GUI(decompInfo->pluginName << " version " << decompInfo->pluginVersion
				<< " can selectively decompile only relocatable objects loaded at 0x0.\n"
				"Rebase the program to 0x0 or use full decompilation or our web interface at: "
				<< decompInfo->pluginURL);
		return;
	}

	// Decompilation triggered by double click.
	//
	if (fnc2decomp)
	{
		decompInfo->navigationList.erase(
				++decompInfo->navigationActual,
				decompInfo->navigationList.end());
		decompInfo->navigationList.push_back(fnc2decomp);
		decompInfo->navigationActual = decompInfo->navigationList.end();
		decompInfo->navigationActual--;

		// Show existing function.
		//
		auto fit = decompInfo->fnc2code.find(fnc2decomp);
		if (!force && fit != decompInfo->fnc2code.end())
		{
			decompInfo->decompiledFunction = fnc2decomp;

			qstring fncName;
			get_func_name(&fncName, fnc2decomp->start_ea);
			INFO_MSG("Show already decompiled function: " << fncName.c_str()
					<< " @ " << std::hex << fnc2decomp->start_ea << "\n");

			//decompInfo->thdProcing = true;
			//decompInfo->curUIthread = qthread_create(showDecompiledCode, static_cast<void*>(&decompInfo));
			ShowOutput show(decompInfo);
			show.execute();

			return;
		}
		// Decompile new function.
		//
		else
		{
			createRangesFromSelectedFunction(*decompInfo, fnc2decomp);
		}
	}
	// Decompilation run from our viewer.
	//
	else if (get_current_viewer() == decompInfo->custViewer
			|| get_current_viewer() == decompInfo->codeViewer)
	{
		// Re-decompile current function.
		//
		if (decompInfo->decompiledFunction)
		{
			createRangesFromSelectedFunction(
					*decompInfo,
					decompInfo->decompiledFunction);

			decompInfo->navigationList.erase(
					decompInfo->navigationActual,
					decompInfo->navigationList.end());
			decompInfo->navigationList.push_back(decompInfo->decompiledFunction);
			decompInfo->navigationActual = decompInfo->navigationList.end();
			decompInfo->navigationActual--;
		}
		// No current function -> something went wrong.
		//
		else
		{
			return;
		}
	}
	// Decompilation run from some other window.
	//
	else
	{
		ea_t addr = get_screen_ea();
		func_t *fnc = get_func(addr);

		// Decompilation run from IDA disasm window (or some other window that allows it).
		//
		if (fnc)
		{
			createRangesFromSelectedFunction(*decompInfo, fnc);
			decompInfo->decompiledFunction = fnc;

			decompInfo->navigationList.clear();
			decompInfo->navigationList.push_back( decompInfo->decompiledFunction );
			decompInfo->navigationActual = decompInfo->navigationList.end();
			decompInfo->navigationActual--;
		}
		// Bad window or bad position in disasm code.
		//
		else
		{
			WARNING_GUI("Function must be selected by the cursor.\n");
			return;
		}
	}

	INFO_MSG("Running Ghidra decompiler plugin:\n");

	saveIdaDatabase();
	generatePluginDatabase();
	decompileInput(*decompInfo);
}

/**
 * Decompile everything, but do not show it in viewer, instead dump it into file.
 */
void runAllDecompilation()
{
	std::string defaultOut = decompInfo->inputPath + ".c";
	char* tmp;
	int iChoice;
	do {
		tmp = ask_file(                ///< Returns: file name
			true,                        ///< bool for_saving
			defaultOut.data(),           ///< const char *default_answer
			"%s",                        ///< const char *format
			"Save decompiled file"
		);

		if (tmp == nullptr) ///< canceled
		{
			return;
		}
		struct stat st;
		if (stat(tmp, &st) == 0) {
			iChoice = ask_yn(ASKBTN_YES, "Overwrite existing file '%s'?", tmp);
			if (iChoice == ASKBTN_CANCEL) return;
		} else iChoice = ASKBTN_YES;
	} while (iChoice == ASKBTN_NO);

	decompInfo->outputFile = tmp;
	decompInfo->ranges.clear();
	decompInfo->decompiledFunction = nullptr;

	INFO_MSG("Selected file: " << decompInfo->outputFile << "\n");

	saveIdaDatabase();
	generatePluginDatabase();
	decompileInput(*decompInfo);
}

/**
 *
 */
bool setInputPath()
{
	char buff[MAXSTR];

	get_root_filename(buff, sizeof(buff));
	std::string inName = buff;

	get_input_file_path(buff, sizeof(buff));
	std::string inPath = buff;

	std::string idb = get_path(PATH_TYPE_IDB);
	std::string id0 = get_path(PATH_TYPE_ID0);
	std::string workDir;
	std::string workIdb;
	if (!idb.empty())
	{
		retdec::utils::FilesystemPath fsIdb(idb);
		workDir = fsIdb.getParentPath();
		workIdb = idb;
	}
	else if (!id0.empty())
	{
		retdec::utils::FilesystemPath fsId0(id0);
		workDir = fsId0.getParentPath();
		workIdb = id0;
	}
	if (workIdb.empty() || workDir.empty())
	{
		WARNING_GUI("Cannot decompile this input file, IDB and ID0 are not set.\n");
		return false;
	}

	if (!retdec::utils::FilesystemPath(inPath).exists())
	{
		INFO_MSG("Input \"" << inPath << "\" does not exist, trying to recover ...\n");

		retdec::utils::FilesystemPath fsWork(workDir);
		fsWork.append(inName);
		workDir = fsWork.getPath();

		inPath = workDir;
		if (!retdec::utils::FilesystemPath(inPath).exists())
		{
			INFO_MSG("Input \"" << inPath << "\" does not exist, asking user to "
					"specify the input file ...\n");

			char *tmp = ask_file(                     ///< Returns: file name
					false,                            ///< bool for_saving
					nullptr,                          ///< const char *default_answer
					"%s",                             ///< const char *format
					"Input binary to decompile"
			);

			if (!tmp)
			{
				return false;
			}
			else if (!retdec::utils::FilesystemPath(std::string(tmp)).exists())
			{
				WARNING_GUI("Cannot decompile this input file, there is no such "
						"file: " << tmp << "\n");
				return false;
			}

			inPath = tmp;

			INFO_MSG("Successfully recovered, using user selected "
					"file \"" << inPath << "\".\n");
		}
		else
		{
			INFO_MSG("Successfully recovered, using input file \"" << inPath << "\".\n");
		}
	}
	else
	{
		INFO_MSG("Working on input file \"" << inPath << "\".\n");
	}

	decompInfo->inputName = inName;
	decompInfo->inputPath = inPath;
	decompInfo->workDir = workDir;
	decompInfo->workIdb = workIdb;

	DBG_MSG("Input Path : " << decompInfo->inputPath << "\n");
	DBG_MSG("Input Name : " << decompInfo->inputName << "\n");
	DBG_MSG("Work dir   : " << decompInfo->workDir << "\n");
	DBG_MSG("Work IDB   : " << decompInfo->workIdb << "\n");

	return true;
}

/**
 * Perform startup check that determines, if plugin can decompile IDA's input file.
 * @return True if plugin can decompile IDA's input, false otherwise.
 * TODO: do some more checking (architecture, ...).
 */
bool canDecompileInput()
{
	if (!setInputPath())
	{
		return false;
	}

	decompInfo->li.clear();
	decompInfo->toolMap.clear();
	DecompInterface::getLangFiles(decompInfo->ghidraPath + "/Ghidra/Processors", "IDA-PRO", decompInfo->toolMap, decompInfo->li);
	//metapc is reported for 16-bit but not reported in Ghidra
	for (size_t i = 0; i < decompInfo->li.size(); i++) {
		if (decompInfo->li[i].processor == "x86" && decompInfo->li[i].size == 16) {
			decompInfo->toolMap["metapc"].push_back((int)i); break;
		}
	}
	return decompInfo->toolMap.find(inf.procname) != decompInfo->toolMap.end();
	/*// 32-bit binary -> is_32bit() == 1 && is_64bit() == 0.
	// 64-bit binary -> is_32bit() == 1 && is_64bit() == 1.
	// Allow 64-bit x86.
	if ((!inf.is_32bit() || inf.is_64bit()) && !isX86())
	{
		WARNING_GUI(decompInfo->pluginName << " version " << decompInfo->pluginVersion
				<< " can decompile only 32-bit input files.\n"
				<< "PROCNAME = " << inf.procname
		);
		return false;
	}

	if (!(inf.filetype == f_BIN
			|| inf.filetype == f_EXE_old
			|| inf.filetype == f_COM_old
			|| inf.filetype == f_LX
			|| inf.filetype == f_LE
			|| inf.filetype == f_WIN
			|| inf.filetype == f_EXE
			|| inf.filetype == f_COM
			|| inf.filetype == f_PE
			|| inf.filetype == f_ELF
			|| inf.filetype == f_COFF
			|| inf.filetype == f_MACHO
			|| inf.filetype == f_HEX))
	{
		if (inf.filetype == f_LOADER)
		{
			WARNING_GUI("Custom IDA loader plugin was used.\n"
					"Decompilation will be attempted, but:\n"
					"1. GhidraDec idaplugin can not check if the input can be "
					"decompiled. Decompilation may fail.\n"
					"2. If the custom loader behaves differently than the GhidraDec "
					"loader, decompilation may fail or produce nonsensical result.");
		}
		else
		{
			WARNING_GUI(decompInfo->pluginName << " version " << decompInfo->pluginVersion
					<< " cannot decompile this input file (file type = "
					<< inf.filetype << ").\n");
			return false;
		}
	}

	decompInfo->mode.clear();
	decompInfo->architecture.clear();
	decompInfo->endian.clear();
	decompInfo->rawEntryPoint = retdec::utils::Address();
	decompInfo->rawSectionVma = retdec::utils::Address();

	// Check Intel HEX.
	//
	if (inf.filetype == f_HEX)
	{
		std::string procName = inf.procname;
		if (procName == "mipsr" || procName == "mipsb")
		{
			decompInfo->architecture = "mips";
			decompInfo->endian = "big";
		}
		else if (procName == "mipsrl"
				|| procName == "mipsl"
				|| procName == "psp")
		{
			decompInfo->architecture = "mips";
			decompInfo->endian = "little";
		}
		else
		{
			WARNING_GUI("Intel HEX input file can be decompiled only for one of "
					"these {mipsr, mipsb, mipsrl, mipsl, psp} processors, "
					"not \"" << procName << "\".\n");
			return false;
		}
	}

	// Check BIN (RAW).
	//
	if (inf.filetype == f_BIN)
	{
		decompInfo->mode = "raw";

		// Section VMA.
		//
		decompInfo->rawSectionVma = inf.min_ea;

		// Entry point.
		//
		if (inf.start_ea != BADADDR)
		{
			decompInfo->rawEntryPoint = inf.start_ea;
		}
		else
		{
			decompInfo->rawEntryPoint = decompInfo->rawSectionVma;
		}

		// Architecture + endian.
		//
		std::string procName = inf.procname;
		if (procName == "mipsr" || procName == "mipsb")
		{
			decompInfo->architecture = "mips";
			decompInfo->endian = "big";
		}
		else if (procName == "mipsrl" || procName == "mipsl" || procName == "psp")
		{
			decompInfo->architecture = "mips";
			decompInfo->endian = "little";
		}
		else if (procName == "ARM")
		{
			decompInfo->architecture = "arm";
			decompInfo->endian = "little";
		}
		else if (procName == "ARMB")
		{
			decompInfo->architecture = "arm";
			decompInfo->endian = "big";
		}
		else if (procName == "PPCL")
		{
			decompInfo->architecture = "powerpc";
			decompInfo->endian = "little";
		}
		else if (procName == "PPC")
		{
			decompInfo->architecture = "powerpc";
			decompInfo->endian = "big";
		}
		else if (isX86())
		{
			decompInfo->architecture = inf.is_64bit() ? "x86-64" : "x86";
			decompInfo->endian = "little";
		}
		else
		{
			WARNING_GUI("Binary input file can be decompiled only for one of these "
					"{mipsr, mipsb, mipsrl, mipsl, psp, ARM, ARMB, PPCL, PPC, 80386p, "
					"80386r, 80486p, 80486r, 80586p, 80586r, 80686p, p2, p3, p4} "
					"processors, not \"" << procName << "\".\n");
			return false;
		}
	}

	return true;*/
}

} // namespace idaplugin

using namespace idaplugin;

/**
 * Plugin run function.
 * The plugin can be passed an integer argument from plugins.cfg file.
 * This can be useful when we want the one plugin to do something
 * different depending on the hot-key pressed or menu item selected.
 * IDA is searching for this function.
 * @param arg Argument set to value according plugins.cfg based on invocation hotkey.
 */
#ifdef __X64__
bool idaapi run(size_t arg)
#else
void idaapi run(int arg)
#endif
{
	bool bRet = true;
	if (!auto_is_ok())
	{
		INFO_MSG("GhidraDec plugin cannot run because the initial autoanalysis has not been finished.\n");
		bRet = false;
	}
	if (decompInfo->decompThread != nullptr) {
		if (!decompInfo->decompRunning) killDecompilation(false);
		else if (ask_buttons(nullptr, nullptr, nullptr, ASKBTN_YES, "HIDECANCEL\nA prior decompilation is still running, would you like to terminate it to process the new request?") == ASKBTN_YES) {
			killDecompilation(true);
		} else bRet = false;
	}


	if (bRet && decompInfo->configureDecompilation())
	{
		bRet = false;
	}

	if (bRet && !canDecompileInput()) //needs configuration first
	{
		bRet = false;
	}

	// ordinary selective decompilation
	//
	if (!bRet) {}
	else if (arg == 0)
	{
		runSelectiveDecompilation();
	}
	// ordinary full decompilation
	//
	else if (arg == 1)
	{
		runAllDecompilation();
	}
	// only plugin configuration
	//
	else if (arg == 2)
	{
		if (!pluginConfigurationMenu(*decompInfo)) {
			killDecompilation(true); //settings do not take effect without restarting process
		}
	}
	// only run database generation
	//
	else if (arg == 3)
	{
		generatePluginDatabase();
	}
	// selective decompilation used in plugin's regression tests
	// forced local decompilation + disabled threads
	// function to decompile is selected by "<ghidradec_select>" string in function's comment
	//
	else if (arg == 4)
	{
		for (unsigned i = 0; i < get_func_qty(); ++i)
		{
			qstring qCmt;
			func_t *fnc = getn_func(i);
			if (get_func_cmt(&qCmt, fnc, false) <= 0)
			{
				continue;
			}

			std::string cmt = qCmt.c_str();;
			if (cmt.find("<ghidradec_select>") != std::string::npos)
			{
				decompInfo->outputFile = decompInfo->inputPath + ".c";
				decompInfo->setIsUseThreads(false);
				runSelectiveDecompilation(fnc);
				break;
			}
		}
	}
	// full decompilation used in plugin's regression tests
	// forced local decompilation + disabled threads
	//
	else if (arg == 5)
	{
		decompInfo->setIsUseThreads(false);
		runAllDecompilation();
	}
	else
	{
		WARNING_GUI(decompInfo->pluginName << " version " << decompInfo->pluginVersion
				<< " cannot handle argument '" << arg << "'.\n");
		bRet = false;
	}
#ifdef __X64__
	return bRet;
#endif
}

/**
 * Plugin initialization function.
 * IDA is searching for this function.
 */
int idaapi init()
{
	static bool inited = false;
	if (inited)
	{
		return PLUGIN_KEEP;
	}
	decompInfo = new RdGlobalInfo();
	decompInfo->pluginRegNumber = register_addon(&decompInfo->pluginInfo);
	if (decompInfo->pluginRegNumber < 0)
	{
		delete decompInfo;
		WARNING_GUI(decompInfo->pluginName << " version " << decompInfo->pluginVersion
				<< " failed to register.\n");
		return PLUGIN_SKIP;
	}
	else
	{
		INFO_MSG(decompInfo->pluginName << " version "
				<< decompInfo->pluginVersion << " registered OK\n");
	}

	readConfigFile(*decompInfo);

	if (is_idaq() && addConfigurationMenuOption(*decompInfo))
	{
		delete decompInfo;
		return PLUGIN_SKIP;
	}

	INFO_MSG(decompInfo->pluginName << " version " << decompInfo->pluginVersion
			<< " loaded OK\n");

	decompInfo->qm = qmutex_create();
	decompInfo->termSem = qsem_create(nullptr, 0);
	hook_to_notification_point(HT_UI, ui_callback, decompInfo);
	registerPermanentActions();
	inited = true;
	return PLUGIN_KEEP;
}

/**
 * Plugin termination function.
 * IDA is searching for this function.
 */
void idaapi term()
{
	killDecompilation(true);
	if (decompInfo->idacb != nullptr) {
		delete decompInfo->idacb;
		decompInfo->idacb = nullptr;
	}
	if (decompInfo->graphWidget != nullptr) {
		close_widget(decompInfo->graphViewer, 0);
		decompInfo->graphViewer = nullptr;
		close_widget(decompInfo->graphWidget, 0);
		decompInfo->graphWidget = nullptr;
		delete_mutable_graph(decompInfo->mg);
		netnode nn("GhidraGraph", 0, true);
		nn.kill();
	}
	if (decompInfo->codeViewer)
	{
		close_widget(decompInfo->codeViewer, 0);
		decompInfo->custViewer = nullptr;
		decompInfo->codeViewer = nullptr;
	}
	unhook_from_notification_point(HT_UI, ui_callback);
	qsem_free(decompInfo->termSem);
	qmutex_free(decompInfo->qm);
	delete decompInfo;
}

/**
 * Plugin interface definition.
 * IDA is searching for this structure.
 */
plugin_t PLUGIN =
{
	IDP_INTERFACE_VERSION,             // Constant version.
	0,                                 // Plugin flags.
	init,                              // Initialize.
	term,                              // Terminate. this pointer may be nullptr.
	run,                               // Invoke plugin.
	PLUGIN_COPYRIGHT, // Long comment about the plugin.
	PLUGIN_URL,       // Multiline help about the plugin.
	PLUGIN_NAME,      // The preferred short name of the plugin.
	PLUGIN_HOTKEY     // The preferred hotkey to run the plugin.
};
