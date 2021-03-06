//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"
#include "Error.h"
#include "ICF.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "SymbolTable.h"
#include "Target.h"
#include "Writer.h"
#include "lld/Driver/Driver.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include <utility>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf;

Configuration *elf::Config;
LinkerDriver *elf::Driver;

bool elf::link(ArrayRef<const char *> Args, raw_ostream &Error) {
  HasError = false;
  ErrorOS = &Error;
  Configuration C;
  LinkerDriver D;
  LinkerScript LS;
  Config = &C;
  Driver = &D;
  Script = &LS;
  Driver->main(Args);
  return !HasError;
}

static std::pair<ELFKind, uint16_t> parseEmulation(StringRef S) {
  if (S == "elf32btsmip")
    return {ELF32BEKind, EM_MIPS};
  if (S == "elf32ltsmip")
    return {ELF32LEKind, EM_MIPS};
  if (S == "elf32ppc" || S == "elf32ppc_fbsd")
    return {ELF32BEKind, EM_PPC};
  if (S == "elf64ppc" || S == "elf64ppc_fbsd")
    return {ELF64BEKind, EM_PPC64};
  if (S == "elf_i386")
    return {ELF32LEKind, EM_386};
  if (S == "elf_x86_64")
    return {ELF64LEKind, EM_X86_64};
  if (S == "aarch64linux")
    return {ELF64LEKind, EM_AARCH64};
  if (S == "i386pe" || S == "i386pep" || S == "thumb2pe")
    error("Windows targets are not supported on the ELF frontend: " + S);
  else
    error("unknown emulation: " + S);
  return {ELFNoneKind, 0};
}

// Returns slices of MB by parsing MB as an archive file.
// Each slice consists of a member file in the archive.
static std::vector<MemoryBufferRef> getArchiveMembers(MemoryBufferRef MB) {
  std::unique_ptr<Archive> File =
      check(Archive::create(MB), "failed to parse archive");

  std::vector<MemoryBufferRef> V;
  for (const ErrorOr<Archive::Child> &COrErr : File->children()) {
    Archive::Child C = check(COrErr, "could not get the child of the archive " +
                                         File->getFileName());
    MemoryBufferRef Mb =
        check(C.getMemoryBufferRef(),
              "could not get the buffer for a child of the archive " +
                  File->getFileName());
    V.push_back(Mb);
  }
  return V;
}

// Opens and parses a file. Path has to be resolved already.
// Newly created memory buffers are owned by this driver.
void LinkerDriver::addFile(StringRef Path) {
  using namespace llvm::sys::fs;
  if (Config->Verbose || Config->Trace)
    llvm::outs() << Path << "\n";
  auto MBOrErr = MemoryBuffer::getFile(Path);
  if (!MBOrErr) {
    error(MBOrErr, "cannot open " + Path);
    return;
  }
  std::unique_ptr<MemoryBuffer> &MB = *MBOrErr;
  MemoryBufferRef MBRef = MB->getMemBufferRef();
  OwningMBs.push_back(std::move(MB)); // take MB ownership

  switch (identify_magic(MBRef.getBuffer())) {
  case file_magic::unknown:
    Script->read(MBRef);
    return;
  case file_magic::archive:
    if (WholeArchive) {
      for (MemoryBufferRef MB : getArchiveMembers(MBRef))
        Files.push_back(createObjectFile(MB, Path));
      return;
    }
    Files.push_back(make_unique<ArchiveFile>(MBRef));
    return;
  case file_magic::elf_shared_object:
    if (Config->Relocatable) {
      error("attempted static link of dynamic object " + Path);
      return;
    }
    Files.push_back(createSharedFile(MBRef));
    return;
  default:
    Files.push_back(createObjectFile(MBRef));
  }
}

// Add a given library by searching it from input search paths.
void LinkerDriver::addLibrary(StringRef Name) {
  std::string Path = searchLibrary(Name);
  if (Path.empty())
    error("unable to find library -l" + Name);
  else
    addFile(Path);
}

// Some command line options or some combinations of them are not allowed.
// This function checks for such errors.
static void checkOptions(opt::InputArgList &Args) {
  // The MIPS ABI as of 2016 does not support the GNU-style symbol lookup
  // table which is a relatively new feature.
  if (Config->EMachine == EM_MIPS && Config->GnuHash)
    error("the .gnu.hash section is not compatible with the MIPS target.");

  if (Config->EMachine == EM_AMDGPU && !Config->Entry.empty())
    error("-e option is not valid for AMDGPU.");

  if (Config->Pie && Config->Shared)
    error("-shared and -pie may not be used together");

  if (!Config->Relocatable)
    return;

  if (Config->Shared)
    error("-r and -shared may not be used together");
  if (Config->GcSections)
    error("-r and --gc-sections may not be used together");
  if (Config->ICF)
    error("-r and --icf may not be used together");
  if (Config->Pie)
    error("-r and -pie may not be used together");
}

static StringRef
getString(opt::InputArgList &Args, unsigned Key, StringRef Default = "") {
  if (auto *Arg = Args.getLastArg(Key))
    return Arg->getValue();
  return Default;
}

static bool hasZOption(opt::InputArgList &Args, StringRef Key) {
  for (auto *Arg : Args.filtered(OPT_z))
    if (Key == Arg->getValue())
      return true;
  return false;
}

void LinkerDriver::main(ArrayRef<const char *> ArgsArr) {
  ELFOptTable Parser;
  opt::InputArgList Args = Parser.parse(ArgsArr.slice(1));
  if (Args.hasArg(OPT_help)) {
    printHelp(ArgsArr[0]);
    return;
  }
  if (Args.hasArg(OPT_version)) {
    printVersion();
    return;
  }

  readConfigs(Args);
  createFiles(Args);
  checkOptions(Args);
  if (HasError)
    return;

  switch (Config->EKind) {
  case ELF32LEKind:
    link<ELF32LE>(Args);
    return;
  case ELF32BEKind:
    link<ELF32BE>(Args);
    return;
  case ELF64LEKind:
    link<ELF64LE>(Args);
    return;
  case ELF64BEKind:
    link<ELF64BE>(Args);
    return;
  default:
    error("-m or at least a .o file required");
  }
}

// Initializes Config members by the command line options.
void LinkerDriver::readConfigs(opt::InputArgList &Args) {
  for (auto *Arg : Args.filtered(OPT_L))
    Config->SearchPaths.push_back(Arg->getValue());

  std::vector<StringRef> RPaths;
  for (auto *Arg : Args.filtered(OPT_rpath))
    RPaths.push_back(Arg->getValue());
  if (!RPaths.empty())
    Config->RPath = llvm::join(RPaths.begin(), RPaths.end(), ":");

  if (auto *Arg = Args.getLastArg(OPT_m)) {
    // Parse ELF{32,64}{LE,BE} and CPU type.
    StringRef S = Arg->getValue();
    std::tie(Config->EKind, Config->EMachine) = parseEmulation(S);
    Config->Emulation = S;
  }

  Config->AllowMultipleDefinition = Args.hasArg(OPT_allow_multiple_definition);
  Config->Bsymbolic = Args.hasArg(OPT_Bsymbolic);
  Config->BsymbolicFunctions = Args.hasArg(OPT_Bsymbolic_functions);
  Config->BuildId = Args.hasArg(OPT_build_id);
  Config->Demangle = !Args.hasArg(OPT_no_demangle);
  Config->DiscardAll = Args.hasArg(OPT_discard_all);
  Config->DiscardLocals = Args.hasArg(OPT_discard_locals);
  Config->DiscardNone = Args.hasArg(OPT_discard_none);
  Config->EhFrameHdr = Args.hasArg(OPT_eh_frame_hdr);
  Config->EnableNewDtags = !Args.hasArg(OPT_disable_new_dtags);
  Config->ExportDynamic = Args.hasArg(OPT_export_dynamic);
  Config->GcSections = Args.hasArg(OPT_gc_sections);
  Config->ICF = Args.hasArg(OPT_icf);
  Config->NoUndefined = Args.hasArg(OPT_no_undefined);
  Config->NoinhibitExec = Args.hasArg(OPT_noinhibit_exec);
  Config->Pie = Args.hasArg(OPT_pie);
  Config->PrintGcSections = Args.hasArg(OPT_print_gc_sections);
  Config->Relocatable = Args.hasArg(OPT_relocatable);
  Config->SaveTemps = Args.hasArg(OPT_save_temps);
  Config->Shared = Args.hasArg(OPT_shared);
  Config->StripAll = Args.hasArg(OPT_strip_all);
  Config->Threads = Args.hasArg(OPT_threads);
  Config->Trace = Args.hasArg(OPT_trace);
  Config->Verbose = Args.hasArg(OPT_verbose);
  Config->WarnCommon = Args.hasArg(OPT_warn_common);

  Config->DynamicLinker = getString(Args, OPT_dynamic_linker);
  Config->Entry = getString(Args, OPT_entry);
  Config->Fini = getString(Args, OPT_fini, "_fini");
  Config->Init = getString(Args, OPT_init, "_init");
  Config->OutputFile = getString(Args, OPT_o);
  Config->SoName = getString(Args, OPT_soname);
  Config->Sysroot = getString(Args, OPT_sysroot);

  Config->ZExecStack = hasZOption(Args, "execstack");
  Config->ZNodelete = hasZOption(Args, "nodelete");
  Config->ZNow = hasZOption(Args, "now");
  Config->ZOrigin = hasZOption(Args, "origin");
  Config->ZRelro = !hasZOption(Args, "norelro");

  Config->Pic = Config->Pie || Config->Shared;

  if (Config->Relocatable)
    Config->StripAll = false;

  if (auto *Arg = Args.getLastArg(OPT_O)) {
    StringRef Val = Arg->getValue();
    if (Val.getAsInteger(10, Config->Optimize))
      error("invalid optimization level");
  }

  if (auto *Arg = Args.getLastArg(OPT_hash_style)) {
    StringRef S = Arg->getValue();
    if (S == "gnu") {
      Config->GnuHash = true;
      Config->SysvHash = false;
    } else if (S == "both") {
      Config->GnuHash = true;
    } else if (S != "sysv")
      error("unknown hash style: " + S);
  }

  for (auto *Arg : Args.filtered(OPT_undefined))
    Config->Undefined.push_back(Arg->getValue());
}

void LinkerDriver::createFiles(opt::InputArgList &Args) {
  for (auto *Arg : Args) {
    switch (Arg->getOption().getID()) {
    case OPT_l:
      addLibrary(Arg->getValue());
      break;
    case OPT_INPUT:
    case OPT_script:
      addFile(Arg->getValue());
      break;
    case OPT_as_needed:
      Config->AsNeeded = true;
      break;
    case OPT_no_as_needed:
      Config->AsNeeded = false;
      break;
    case OPT_Bstatic:
      Config->Static = true;
      break;
    case OPT_Bdynamic:
      Config->Static = false;
      break;
    case OPT_whole_archive:
      WholeArchive = true;
      break;
    case OPT_no_whole_archive:
      WholeArchive = false;
      break;
    }
  }

  if (Files.empty() && !HasError)
    error("no input files.");
}

template <class ELFT> static void initSymbols() {
  ElfSym<ELFT>::Etext.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::Edata.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::End.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::Ignored.setBinding(STB_WEAK);
  ElfSym<ELFT>::Ignored.setVisibility(STV_HIDDEN);
}

template <class ELFT> void LinkerDriver::link(opt::InputArgList &Args) {
  // For LTO
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  initSymbols<ELFT>();

  SymbolTable<ELFT> Symtab;
  std::unique_ptr<TargetInfo> TI(createTarget());
  Target = TI.get();

  Config->Rela = ELFT::Is64Bits;

  // Add entry symbol.
  // There is no entry symbol for AMDGPU binaries, so skip adding one to avoid
  // having and undefined symbol.
  if (Config->Entry.empty() && !Config->Shared && !Config->Relocatable &&
      Config->EMachine != EM_AMDGPU)
    Config->Entry = Config->EMachine == EM_MIPS ? "__start" : "_start";

  // In the assembly for 32 bit x86 the _GLOBAL_OFFSET_TABLE_ symbol
  // is magical and is used to produce a R_386_GOTPC relocation.
  // The R_386_GOTPC relocation value doesn't actually depend on the
  // symbol value, so it could use an index of STN_UNDEF which, according
  // to the spec, means the symbol value is 0.
  // Unfortunately both gas and MC keep the _GLOBAL_OFFSET_TABLE_ symbol in
  // the object file.
  // The situation is even stranger on x86_64 where the assembly doesn't
  // need the magical symbol, but gas still puts _GLOBAL_OFFSET_TABLE_ as
  // an undefined symbol in the .o files.
  // Given that the symbol is effectively unused, we just create a dummy
  // hidden one to avoid the undefined symbol error.
  if (!Config->Relocatable)
    Symtab.addIgnored("_GLOBAL_OFFSET_TABLE_");

  if (!Config->Entry.empty()) {
    // Set either EntryAddr (if S is a number) or EntrySym (otherwise).
    StringRef S = Config->Entry;
    if (S.getAsInteger(0, Config->EntryAddr))
      Config->EntrySym = Symtab.addUndefined(S);
  }

  if (Config->EMachine == EM_MIPS) {
    // On MIPS O32 ABI, _gp_disp is a magic symbol designates offset between
    // start of function and 'gp' pointer into GOT.
    Config->MipsGpDisp = Symtab.addIgnored("_gp_disp");
    // The __gnu_local_gp is a magic symbol equal to the current value of 'gp'
    // pointer. This symbol is used in the code generated by .cpload pseudo-op
    // in case of using -mno-shared option.
    // https://sourceware.org/ml/binutils/2004-12/msg00094.html
    Config->MipsLocalGp = Symtab.addIgnored("__gnu_local_gp");

    // Define _gp for MIPS. st_value of _gp symbol will be updated by Writer
    // so that it points to an absolute address which is relative to GOT.
    // See "Global Data Symbols" in Chapter 6 in the following document:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    Symtab.addAbsolute("_gp", ElfSym<ELFT>::MipsGp);
  }

  for (std::unique_ptr<InputFile> &F : Files)
    Symtab.addFile(std::move(F));
  if (HasError)
    return; // There were duplicate symbols or incompatible files

  for (StringRef S : Config->Undefined)
    Symtab.addUndefinedOpt(S);

  Symtab.addCombinedLtoObject();

  for (auto *Arg : Args.filtered(OPT_wrap))
    Symtab.wrap(Arg->getValue());

  if (Config->OutputFile.empty())
    Config->OutputFile = "a.out";

  // Write the result to the file.
  Symtab.scanShlibUndefined();
  if (Config->GcSections)
    markLive<ELFT>(&Symtab);
  if (Config->ICF)
    doIcf<ELFT>(&Symtab);
  writeResult<ELFT>(&Symtab);
}
