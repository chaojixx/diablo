/* This research is supported by the European Union Seventh Framework Programme (FP7/2007-2013), project ASPIRE (Advanced  Software Protection: Integration, Research, and Exploitation), under grant agreement no. 609734; on-line at https://aspire-fp7.eu/. */

/* The development of portions of the code contained in this file was sponsored by Samsung Electronics UK. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <random>
#include <string>

extern "C" {
#include <diabloanopt.h>
// #include <diablodiversity_cmdline.h>

#include "diablo_options.h"
#include "obfuscation_opt.h"
}

#include <diabloflowgraph_dwarf.h>

#include <vector>
#include <fstream>
#include <iostream>
#include <set>

#include "obfuscation_architecture_backend.h"
#include "obfuscation_transformation.h"

/* To init the logging */
#include "generic/branch_function.h"
#include "generic/flatten_function.h"
#include "generic/opaque_predicate.h"

#include <diabloannotations.h>
#include "obfuscation_json.h"

using namespace std;

/* For now, the format of the file is, lines describing transformations (transformations applied as they are read from the file):
 * function <function_name> <function_transformation_name>
 * bbls <comma-separated-list-of-BBL-addresses-in-hexadecimal> <percentage-to-be-transformed-as-integer-between-0-100> <bbl_transformation_name>
 */
void ObjectObfuscateFromScript(t_string script_file, t_string filename, t_object* obj) {
  ifstream script(script_file);
  t_cfg* cfg = OBJECT_CFG(obj);
  t_randomnumbergenerator *rng_obfuscation = RNGCreateChild(RNGGetRootGenerator(), "obfuscations");
  t_randomnumbergenerator *rng_bbl = RNGCreateChild(rng_obfuscation, "bbl_obfuscation");
  t_randomnumbergenerator *rng_fun = RNGCreateChild(rng_obfuscation, "function_obfuscation");

  VERBOSE(0, ("Obfuscating with script file '%s'", script_file));
  SetAllObfuscationsEnabled(true);

  /* TODO: error checking/handling */
  while(!script.eof()) {
    string read;
    script >> read;

    if (read == "function") {
      string fun_name, transformation;

      script >> fun_name >> transformation;

      VERBOSE(0, ("Transforming: function '%s' with transformation '%s'", fun_name.c_str(), transformation.c_str()));

      bool found = false;
      /* TODO: overhead */
      t_function* fun;
      t_function* fun_safe;
      CFG_FOREACH_FUNCTION_SAFE(cfg, fun, fun_safe) {
        if (FUNCTION_NAME(fun) && fun_name == FUNCTION_NAME(fun)) {
          found = true;

          FunctionObfuscationTransformation* obfuscator = GetRandomTypedTransformationForType<FunctionObfuscationTransformation>(transformation.c_str(), rng_fun);
          ASSERT(obfuscator, ("Did not find any obfuscator for type '%s'", transformation.c_str()));

          if (obfuscator->canTransform(fun)) {
            VERBOSE(1, ("Applying '%s' to '%s'", obfuscator->name(), FUNCTION_NAME(fun)));
            obfuscator->doTransform(fun, rng_fun);
          } else {
            VERBOSE(0, ("WARNING: tried applying '%s' to '%s', but FAILED", obfuscator->name(), FUNCTION_NAME(fun)));
          }
          /* Don't break, we can have duplicated a BBL, potentially transform all duplicates */
        }
      }
      if (!found) {
        VERBOSE(0, ("WARNING! Did not find a function named '%s'", fun_name.c_str()));
      }
    } else if (read == "bbls") {
      set<t_uint32> addrs;
      t_uint32 addr;
      script >> hex;
      bool has_next = false;
      do {
        script >> addr;
        has_next = script.get() == ',';

        addrs.insert(addr);
      } while (has_next);

      int pct;
      string transformation;

      script >> dec >> pct;
      script >> transformation;

      VERBOSE(0, ("Transforming: BBLs with pct %i and obfuscation '%s':", pct, transformation.c_str()));

      for (auto addr: addrs) {
        VERBOSE(1, ("Transforming bbl at 0x%x", addr));

        t_bbl* bbl;
        bool found = false;
        /* TODO: overhead */
        CFG_FOREACH_BBL(cfg, bbl) {
          if (BBL_CADDRESS(bbl) == addr) {
            found = true;

            BBLObfuscationTransformation* obfuscator = GetRandomTypedTransformationForType<BBLObfuscationTransformation>(transformation.c_str(), rng_bbl);
            ASSERT(obfuscator, ("Did not find any obfuscator for type '%s'", transformation.c_str()));

            if (obfuscator->canTransform(bbl)) {
              VERBOSE(1, ("Applying '%s' to @eiB", obfuscator->name(), bbl));
              obfuscator->doTransform(bbl, rng_bbl);
            } else {
              VERBOSE(0, ("WARNING: tried applying '%s' to @eiB, but FAILED", obfuscator->name(), bbl));
            }
            /* Don't break, we can have duplicated a BBL, potentially transform all duplicates */
          }
        }

        if (!found) {
          VERBOSE(0, ("WARNING! Did not find a BBL for address %x", addr));
        }
      }
    } else if (read == "") {
      /* ignore */
    } else {
      FATAL(("Unrecognized script line starting with '%s'", read.c_str()));
    }
  }

  RNGDestroy(rng_bbl);
  RNGDestroy(rng_fun);
  RNGDestroy(rng_obfuscation);
  VERBOSE(0, ("DONE Obfuscating with script"));
  VERBOSE(0, ("WARNING TODO: use the pct parameter"));
}


static t_bool
has_incoming_interproc (t_bbl * bbl)
{
  t_cfg_edge *edge;

  BBL_FOREACH_PRED_EDGE(bbl, edge)
    if (CfgEdgeIsForwardInterproc (edge))
      return TRUE;
    else if (CfgEdgeIsInterproc (edge))
    {
      /* backward interprocedural edge: check if we
       * have an interprocedural call/return pair */
      if (BBL_FUNCTION(CFG_EDGE_HEAD(CFG_EDGE_CORR(edge)))
          != BBL_FUNCTION(bbl))
        return TRUE;
    }
  return FALSE;
}

static t_bool
always_true (t_bbl * bbl1, t_bbl * bbl2)
{
  return TRUE;
}


LogFile* L_OBF_OP=NULL;
int
main (int argc, char **argv)
{
  t_object *obj;

  void *obf_arch;
  InitArchitecture(&obf_arch);

  ObfuscationArchitectureInitializer* obfuscationArchitecture = GetObfuscationArchitectureInitializer();

  /* Process command line parameters for each of the used libraries */
  obfuscationArchitecture->Init(argc, argv);
  AddOptionsListInitializer(obfuscation_obfuscation_option_list); ObfuscationOptInit();

  ParseRegisteredOptionLists(argc, argv);

  /* The final option parsing should have TRUE as its last argument */
  GlobalInit ();
  OptionParseCommandLine (global_list, argc, argv, TRUE /* final */);
  OptionGetEnvironment (global_list);
  GlobalVerify ();
  OptionDefaults (global_list);

  PrintVersionInformationIfRequested();

  /* III. The REAL program {{{ */
  if (global_options.random_overrides_file)
    RNGReadOverrides(global_options.random_overrides_file);

  if (!global_options.objectfilename) {
    VERBOSE(0, ("Diablo needs at least an input objectfile/executable as argument to do something."));
    return -1;
  }

  t_string logging_filename = StringConcat2 (global_options.output_name, ".diablo.obfuscation.log");

  LogFile* L_OBF = NULL;
  INIT_LOGGING(L_OBF,logging_filename);
  ATTACH_LOGGING(L_OBF_BF, L_OBF);
  ATTACH_LOGGING(L_OBF_FF, L_OBF);
  ATTACH_LOGGING(L_OBF_OOP, L_OBF);

  LOG(L_OBF, ("START OF OBFUSCATION LOG\n"));

  if (global_options.read)
  {
    ASSERT(global_options.objectfilename, ("Input objectfile/executable name not set!"));
    diabloobject_options.read_debug_info = TRUE;
    TryParseSymbolTranslation(global_options.objectfilename);

    RNGSetRootGenerator(RNGCreateBySeed(global_options.randomseed, "master"));

    obj = LinkEmulate (global_options.objectfilename,diabloobject_options.read_debug_info);

    if(diabloobject_options.read_debug_info)
      DwarfFlowgraphInit();

    /* Read in the annotations from the JSON file if one is provided */
    Annotations annotations;
    if (global_options.annotation_file)
    {
      RegisterAnnotationInfoFactory(obfuscations_token, new ObfuscationAnnotationInfoFactory());
      ReadAnnotationsFromJSON(global_options.annotation_file, annotations);
    }

    if (global_options.self_profiling)
      SelfProfilingInit(obj, global_options.self_profiling);

	//RelocTableRemoveConstantRelocs(OBJECT_RELOC_TABLE(obj));

    if (global_options.disassemble)
    {
      /* B. Transform and optimize  {{{ */
      /* 1. Disassemble */

      ObjectDisassemble (obj);

      /* 2. {{{ Create the flowgraph */
      if (global_options.flowgraph)
      {
        t_cfg *cfg;
        if (global_options.self_profiling)
          {
            t_const_string force_reachable[2];
            force_reachable[0] = FINAL_PREFIX_FOR_LINKED_IN_PROFILING_OBJECT "print";
            force_reachable[1] = NULL;
            ObjectFlowgraph (obj, NULL, force_reachable, FALSE);
          }
        else
          ObjectFlowgraph (obj, NULL, NULL, FALSE);
        cfg = OBJECT_CFG(obj);

        if(diabloobject_options.read_debug_info)
          {
            DiabloBrokerCall("ObfuscationBackendAssociateDWARFWithCFG", cfg);
          }

        DiabloBrokerCall("StucknessAnalysis", obj);

        CfgRemoveDeadCodeAndDataBlocks (cfg);
        CfgPatchToSingleEntryFunctions (cfg);
        CfgRemoveDeadCodeAndDataBlocks (cfg);

        RegionsInit(annotations, cfg);

        if (diabloflowgraph_options.blockprofilefile)
          {
            CfgReadBlockExecutionCounts (cfg,
                                         diabloflowgraph_options.blockprofilefile);
            printf ("START WEIGHT %" PRId64 "\n", CfgComputeWeight (cfg));
            if (diabloflowgraph_options.insprofilefile)
              {
                CfgReadInsExecutionCounts (cfg,
                                           diabloflowgraph_options.insprofilefile);
                CfgComputeHotBblThreshold (cfg, 0.90);
              }
          }

        // MakeConstProducers (cfg);

        VERBOSE(0,("INITIAL PROGRAM COMPLEXITY REPORT"));

        CfgComputeStaticComplexity(cfg);
        CfgComputeDynamicComplexity(cfg);

        //CfgDrawFunctionGraphsWithHotness (OBJECT_CFG(obj), "./dots-before");

        if (global_options.annotation_file == NULL)
          ObjectObfuscate(global_options.objectfilename, obj);
        else
          CfgObfuscateRegions(cfg);

        VERBOSE(0,("FINAL PROGRAM COMPLEXITY REPORT"));

        if (global_options.self_profiling)
          {
            CfgAssignUniqueOldAddresses(cfg);
            NewDiabloPhase("Self-profiling");
            CfgComputeLiveness (cfg, CONTEXT_SENSITIVE);/* We'll need this later on */
            CfgAddSelfProfiling (obj, global_options.output_name);
          }

        CfgComputeStaticComplexity(cfg);
        CfgComputeDynamicComplexity(cfg);

        /* Export dots after optimzation {{{  */
        if (global_options.generate_dots)
          {
            if (diabloflowgraph_options.blockprofilefile)
              CfgComputeHotBblThreshold (cfg, 0.90);
            CfgDrawFunctionGraphsWithHotness (OBJECT_CFG(obj), "./dots-final");
            CgBuild (cfg);
            CgExport (CFG_CG(cfg), "./dots-final/callgraph.dot");
          }
        /* }}} */

        ObjectDeflowgraph (obj);

        if (global_options.print_listing)
          ObjectPrintListing (obj, global_options.output_name);

        /* rebuild the layout of the data sections
         * so that every subsection sits at it's new address */
        ObjectRebuildSectionsFromSubsections (obj);

        RegionsFini(cfg);
      }
      /*  }}} */

#if 0
	{
	  FILE *f = fopen ("mapping.xml", "w");
	  FileIo(f,"<mapping>");
	  if (f)
	  {
	    t_ins *ins;
	    SECTION_FOREACH_INS (OBJECT_CODE (obj)[0], ins)
	    {
        t_address_item * item = INS_ADDRESSLIST(ins)->first;
		FileIo (f, "<ins address=\"@G\" ", INS_CADDRESS(ins));

          FileIo(f, "orig_function_first=\"@G\" guessed_train_exec_count=\"%lli\" >", INS_ORIGINAL_ADDRESS(ins), INS_EXECCOUNT(ins));

        while(item) {
		      FileIo (f, "@G ", item->address);
		      item = item->next;
		    }

	      FileIo (f, "</ins>\n");
	    }
	    FileIo(f,"</mapping>");
	    fclose (f);
	  }
	}
#endif

      ObjectAssemble (obj);
      /* End Transform and optimize }}} */
    }

    AnnotationsDestroy(annotations);
    UnregisterAllAnnotationInfoFactories();


    /* Clean and write out the object {{{ */
    /* remove local definitions of BuildAttributes$$*: the global one from the final
     * linked binary is the combination of all of those */
    t_const_string strip_symbol_masks[3] = {"BuildAttributes$$*", "$switch", NULL};
    ObjectConstructFinalSymbolTable(obj, strip_symbol_masks);

    ObjectWrite (obj, global_options.output_name);

#ifdef DIABLOSUPPORT_HAVE_STAT
    /* make the file executable */
    chmod (global_options.output_name,
	S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH |
	S_IXOTH);
#endif

  }
  /* END REAL program }}} */

  /* IV. Free all structures {{{ */
  /* remove directory that held the restored dump */
  /*if (global_options.restore) RmdirR("./DUMP"); */

  DestroyArchitecture(&obf_arch);
  GlobalFini();
  DiabloFlowgraphFini ();

  /* If DEBUG MALLOC is defined print a list of all memory
   * leaks. Needs to increase the verbose-level to make
   * sure these get printed. This is a hack but it may be
   * removed when a real version of diablo is made */

#ifdef DEBUG_MALLOC
  diablosupport_options.verbose++;
  PrintRemainingBlocks ();
#endif

  LOG(L_OBF,("END OF OBFUSCATION LOG\n"));
  FINI_LOGGING(L_OBF);
  FINI_LOGGING(L_OBF_BF);
  FINI_LOGGING(L_OBF_FF);
  FINI_LOGGING(L_OBF_OOP);

  //scanf("%s", NULL);

  /* End Fini }}} */
  return 0;
}
/* vim: set shiftwidth=2 foldmethod=marker:*/
