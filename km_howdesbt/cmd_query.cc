// cmd_query.cc-- query a sequence bloom tree

#include <string>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <vector>
#include <tuple>

#include "utilities.h"
#include "bit_vector.h"
#include "bloom_filter.h"
#include "bloom_tree.h"
#include "file_manager.h"
#include "query.h"

#include "support.h"
#include "commands.h"
#include "cmd_query.h"

using std::string;
using std::vector;
using std::pair;
using std::tuple;
using std::cin;
using std::cout;
using std::cerr;
using std::endl;
#define u32 std::uint32_t
#define u64 std::uint64_t


void QueryCommand::short_description
   (std::ostream& s)
	{
	s << commandName << "-- query a sequence bloom tree" << endl;
	}

void QueryCommand::usage
   (std::ostream& s,
	const string& message)
	{
	if (!message.empty())
		{
		s << message << endl;
		s << endl;
		}

//$$$ add an option to limit the number of bits used in each BF
//$$$ .. that's to let us experiment with different reductions of BF fraction
//$$$ .. without having to generate every populated filter size; implementation
//$$$ .. would just act as a filter on the hashed position list for each query
	short_description(s);
	s << "usage: " << commandName << " [<queryfilename>[=<F>]] [options]" << endl;
	//    123456789-123456789-123456789-123456789-123456789-123456789-123456789-123456789
	s << "  --tree=<filename>    name of the tree toplogy file" << endl;
	s << "  <queryfilename>      (cumulative) name of a query file; this is either a" << endl;
	s << "                       fasta file or a file with one nucleotide sequence per" << endl;
	s << "                       line; if no query files are provided, queries are read" << endl;
	s << "                       from stdin" << endl;
	s << "  <queryfilename>=<F>  query file with associated threshold; <F> has the same" << endl;
	s << "                       meaning as in --threshold=<F> but applies only to this" << endl;
	s << "                       query file" << endl;
	s << "  --repart=<F>         minimizers repartition (from kmtricks)" << endl;
    s << "  --win=<F>            hash window (from kmtricks)" << endl;
    s << "  --threshold=<F>      fraction of query kmers that must be present in a leaf" << endl;
	s << "                       to be considered a match; this must be between 0 and 1;" << endl;
	s << "                       this only applies to query files for which <F> is not" << endl;
	s << "                       otherwise specified (by <queryfilename>=<F>)" << endl;
	s << "                       (default is " << defaultQueryThreshold << ")" << endl;
	s << "  --sort               sort matched leaves by the number of query kmers present," << endl;
	s << "                       and report the number of kmers present" << endl;
	s << "                       (by default we just report the matched leaves without" << endl;
	s << "                       regard to which matches are better)" << endl;
	s << "  --consistencycheck   before searching, check that bloom filter properties are" << endl;
	s << "                       consistent across the tree" << endl;
	s << "                       (not needed with --usemanager)" << endl;
	s << "  --time               report wall time and node i/o time" << endl;
	s << "  --out=<filename>     file for query results; if this is not provided, results" << endl;
	s << "                       are written to stdout" << endl;
// (no longer advertised -- order_query_results.sh isn't part of the distribution)
//	s << "  --backwardcompatible (requires --adjust or --sort) output is backward" << endl;
//	s << "                       compatible with order_query_results.sh" << endl;
	}

void QueryCommand::debug_help
   (std::ostream& s)
	{
	s << "--debug= options" << endl;
	s << "  trackmemory" << endl;
	s << "  reportfilebytes" << endl;
	s << "  countfilebytes" << endl;
	s << "  reportopenclose" << endl;
	s << "  reportrankselect" << endl;
	s << "  btunload" << endl;
	s << "  bvcreation" << endl;
	s << "  topology" << endl;
	s << "  fmcontentload" << endl;
	s << "  namemapping" << endl;
	s << "  load" << endl;
	s << "  reportloadtime" << endl;
	s << "  reporttotalloadtime" << endl;
	s << "  names" << endl;
	s << "  input" << endl;
	s << "  sort" << endl;
	s << "  kmerize" << endl;
	s << "  kmerizeall" << endl;
	s << "  traversal" << endl;
	s << "  lookups" << endl;
	s << "  positions" << endl;
	s << "  positionsbyhash" << endl;
	s << "  adjustposlist" << endl;
	s << "  rankselectlookup" << endl;
	}

void QueryCommand::parse
   (int		_argc,
	char**	_argv)
	{
	int		argc;
	char**	argv;

	// defaults

	generalQueryThreshold   = -1.0;		// (unassigned threshold)
	sortByKmerCounts        = false;
	distinctKmers           = false;
	checkConsistency        = false;
	justReportKmerCounts    = false;
	reportNodesExamined     = false;
	reportTime              = false;

	// skip command name

	argv = _argv+1;  argc = _argc - 1;
	if (argc <= 0) chastise ();

	//////////
	// scan arguments
	//////////

	for (int argIx=0 ; argIx<argc ; argIx++)
		{
		string arg = argv[argIx];
		string argVal;
		if (arg.empty()) continue;

		string::size_type argValIx = arg.find('=');
		if (argValIx == string::npos) argVal = "";
		                         else argVal = arg.substr(argValIx+1);

		// --help, etc.

		if ((arg == "--help")
		 || (arg == "-help")
		 || (arg == "--h")
		 || (arg == "-h")
		 || (arg == "?")
		 || (arg == "-?")
		 || (arg == "--?"))
			{ usage (cerr);  std::exit (EXIT_SUCCESS); }

		if ((arg == "--help=debug")
		 || (arg == "--help:debug")
		 || (arg == "?debug"))
			{ debug_help(cerr);  std::exit (EXIT_SUCCESS); }

		// --tree=<filename>, etc.

		if ((is_prefix_of (arg, "--tree="))
		 ||	(is_prefix_of (arg, "--intree="))
		 ||	(is_prefix_of (arg, "--topology=")))
			{ treeFilename = argVal;  continue; }

		// (unadvertised) --query=<filename>
		//             or --query=<filename>=<F> or --query=<filename>:<F>

		if (is_prefix_of (arg, "--query="))
			{
			string::size_type threshIx = argVal.find('=');
			if (threshIx == string::npos) threshIx = argVal.find(':');

			if (threshIx == string::npos)
				{
				queryFilenames.emplace_back(strip_blank_ends(argVal));
				queryThresholds.emplace_back(-1.0);  // (unassigned threshold)
				}
			else
				{
				double thisQueryThreshold = string_to_probability(arg.substr(threshIx+1));
				queryFilenames.emplace_back(strip_blank_ends(argVal));
				queryThresholds.emplace_back(thisQueryThreshold);
				}
			continue;
			}

        if (is_prefix_of(arg, "--repart="))
        {
			repartFileName = argVal;
			continue;
        }

        if (is_prefix_of(arg, "--win="))
        {
			winFileName = argVal;
			continue;
        }

		// --threshold=<F>

		if ((is_prefix_of (arg, "--threshold="))
		 ||	(is_prefix_of (arg, "--query-threshold="))
		 ||	(is_prefix_of (arg, "--theta="))
		 ||	(is_prefix_of (arg, "--specificity=")))
			{
			if (generalQueryThreshold >= 0.0)
				{
				cerr << "warning: --threshold=<F> used more that once; only final setting will apply" << endl;
				cerr << "(to use different thresholds for different files, use <queryfilename>=<F> form)" << endl;
				}
			generalQueryThreshold = string_to_probability(argVal);
			continue;
			}

		// --sort

		if (arg == "--sort")
			{ sortByKmerCounts = true;  continue; }


		// --distinctkmers

		if ((arg == "--distinctkmers")
		 || (arg == "--distinct-kmers")
		 || (arg == "--distinct"))
			{ distinctKmers = true;  continue; }

		// --consistencycheck, (unadvertised) --noconsistency

		if (arg == "--consistencycheck")
			{ checkConsistency = true;  continue; }

		if ((arg == "--noconsistency")
		 || (arg == "--noconsistencycheck"))
			{ checkConsistency = false;  continue; }

		// --justcountkmers

		if (arg == "--justcountkmers")
			{
			justReportKmerCounts = true;
			continue;
			}


		// --stat:nodesexamined

		if ((arg == "--stat:nodesexamined")
		 || (arg == "--stats:nodesexamined")
		 || (arg == "--nodesexamined"))
			{ reportNodesExamined = true;  continue; }

		
		// --time

		if ((arg == "--time")
		 || (arg == "--walltime"))
			{ reportTime = true;  continue; }

		// --out=<filename>, etc.

		if ((is_prefix_of (arg, "--out="))
		 ||	(is_prefix_of (arg, "--output="))
		 ||	(is_prefix_of (arg, "--matches="))
		 ||	(is_prefix_of (arg, "--results=")))
			{ matchesFilename = argVal;  continue; }

		// (unadvertised) debug options

		if (arg == "--debug")
			{ debug.insert ("debug");  continue; }

		if (is_prefix_of (arg, "--debug="))
			{
		    for (const auto& field : parse_comma_list(argVal))
				debug.insert(to_lower(field));
			continue;
			}

		// unrecognized --option

		if (is_prefix_of (arg, "--"))
			chastise ("unrecognized option: \"" + arg + "\"");

		// <queryfilename>=<F> or <queryfilename>:<F>

		string::size_type threshIx = argValIx;
		if (threshIx == string::npos) threshIx = arg.find(':');

		if (threshIx != string::npos)
			{
			double thisQueryThreshold = string_to_probability(arg.substr(threshIx+1));
			queryFilenames.emplace_back(strip_blank_ends(arg.substr(0,threshIx)));
			queryThresholds.emplace_back(thisQueryThreshold);
			continue;
			}

		// <queryfilename>

		queryFilenames.emplace_back(strip_blank_ends(arg));
		queryThresholds.emplace_back(-1.0);  // (unassigned threshold)
		}

	// sanity checks

	if (treeFilename.empty())
		chastise ("you have to provide a tree topology file");


	if ((justReportKmerCounts) and (sortByKmerCounts))
		chastise ("--sort cannot be used with --justcountkmers");


	completeKmerCounts = sortByKmerCounts;

	// assign threshold to any unassigned queries

	if (generalQueryThreshold < 0.0)
		generalQueryThreshold = defaultQueryThreshold;

	int numQueryFiles = queryFilenames.size();
	for (int queryIx=0 ; queryIx<numQueryFiles ; queryIx++)
		{
		if (queryThresholds[queryIx] < 0)
			queryThresholds[queryIx] = generalQueryThreshold;
		}

	return;
	}

QueryCommand::~QueryCommand()
	{
	for (const auto& q : queries)
		delete q;
	}

int QueryCommand::execute()
	{
	wall_time_ty startTime;
	if (reportTime) startTime = get_wall_time();

	if (contains(debug,"trackmemory"))
		{
		FileManager::trackMemory = true;
		BloomTree::trackMemory   = true;
		BloomFilter::trackMemory = true;
		BitVector::trackMemory   = true;
		}
	if (contains(debug,"reportfilebytes"))
		{
		BloomFilter::reportFileBytes = true;
		BitVector::reportFileBytes   = true;
		}
	if (contains(debug,"countfilebytes"))
		{
		BloomFilter::countFileBytes = true;
		BitVector::countFileBytes   = true;
		}
	if (contains(debug,"reportopenclose"))
		FileManager::reportOpenClose = true;
	if (contains(debug,"reportrankselect"))
		BitVector::reportRankSelect = true;
	if (contains(debug,"btunload"))
		BloomTree::reportUnload = true;
	if (contains(debug,"bvcreation"))
		BitVector::reportCreation = true;

	// read the tree

	BloomTree* root = BloomTree::read_topology(treeFilename);
	useFileManager = root->nodesShareFiles;

	vector<BloomTree*> order;

	if (contains(debug,"topology"))
		{
		if (useFileManager)
			root->print_topology(cerr,/*level*/0,/*format*/topofmt_containers);
		else
			root->print_topology(cerr,/*level*/0,/*format*/topofmt_nodeNames);
		}

	if (contains(debug,"reportloadtime"))
		{
		BloomFilter::reportLoadTime = true;
		BitVector::reportLoadTime   = true;
		}

	if ((reportTime) || (contains(debug,"reporttotalloadtime")))
		{
		BloomFilter::reportTotalLoadTime = true;
		BitVector::reportTotalLoadTime   = true;
		}

	if (contains(debug,"load"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->reportLoad = true;
		}

	// set up the file manager

	FileManager* manager = nullptr;
	if (useFileManager)
		{
		if (contains(debug,"fmcontentload"))
			FileManager::dbgContentLoad = true;

		manager = new FileManager(root,/*validateConsistency*/false);
		if (contains(debug,"load"))
			manager->reportLoad = true;
		if (contains(debug,"namemapping"))
			{
			for (auto iter : manager->filenameToNames)
				{
				string          filename  = iter.first;
				vector<string>* nodeNames = iter.second;
				cerr << filename << " contains:" << endl;
				for (const auto& nodeName : *nodeNames)
					cerr << "  " << nodeName << endl;
				}
			}
		}

	// if we're not using a file manager, we may still want to do a consistency
	// check before we start the search (we'd rather not run for a long time
	// and *then* report the problem)

	else if (checkConsistency)
		{
		BloomFilter* modelBf = nullptr;

		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			{
			node->preload();

			if (modelBf == nullptr)
				modelBf = node->bf;
			else
				node->bf->is_consistent_with (modelBf, /*beFatal*/ true);
			}
		}

	// read the queries

	read_queries ();

	if (contains(debug,"input"))
		{
		for (auto& q : queries)
			{
			cerr << ">" << q->name << endl;
			cerr << q->seq << endl;
			}
		}

	// propagate debug information into the queries and/or tree nodes



	if ((contains(debug,"traversal"))
	 || (contains(debug,"lookups")))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			{
			node->dbgTraversal = (contains(debug,"traversal"));
			node->dbgLookups   = (contains(debug,"lookups"));
			}
		}

	if (contains(debug,"sort"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->dbgSortKmerPositions = true;
		}

	if (contains(debug,"positions"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->dbgKmerPositions = true;
		}

	if (contains(debug,"positionsbyhash"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->dbgKmerPositionsByHash = true;
		}

	if (contains(debug,"adjustposlist"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->dbgAdjustPosList = true;
		}

	if (contains(debug,"rankselectlookup"))
		{
		if (order.size() == 0)
			root->post_order(order);
		for (const auto& node : order)
			node->dbgRankSelectLookup = true;
		}

	// perform the query (or just report kmer counts)

	if (justReportKmerCounts)
		{
		BloomFilter* bf = root->real_filter();
		for (auto& q : queries)
			{
			q->kmerize(bf,distinctKmers);
			cout << q->name << " " << q->kmerPositions.size() << endl;
			}
		}
	else 
		{
		// perform the query

		root->batch_query(queries,distinctKmers,completeKmerCounts);

		// report results

		if (sortByKmerCounts)
			sort_matches_by_kmer_counts();

		if (matchesFilename.empty())
			{
			if (completeKmerCounts)
				print_matches_with_kmer_counts (cout);
			else
				print_matches (cout);
			}
		else
			{
			std::ofstream out(matchesFilename);
			if (completeKmerCounts)
				print_matches_with_kmer_counts (out);
			else
				print_matches (out);
			}
		}

//$$$ where do we delete the tree?  looks like a memory leak

	FileManager::close_file();	// make sure the last bloom filter file we
								// .. opened for read gets closed

	if (manager != nullptr)
		delete manager;

	if (contains(debug,"countfilebytes"))
		{
		u64 fileReads     = BloomFilter::totalFileReads;
		u64 fileBytesRead = BloomFilter::totalFileBytesRead;
		if (BloomFilter::totalFileReads == 0)
			cerr << "BF fileBytesRead: " << fileBytesRead << "/0" << endl;
		else
			cerr << "BF fileBytesRead: " << fileBytesRead << "/" << fileReads
			     << " (" << (u64) floor(fileBytesRead/fileReads) << " bytes per)" << endl;

		fileReads     = BitVector::totalFileReads;
		fileBytesRead = BitVector::totalFileBytesRead;
		if (fileReads == 0)
			cerr << "BV fileBytesRead: " << fileBytesRead << "/0" << endl;
		else
			cerr << "BV fileBytesRead: " << fileBytesRead << "/" << fileReads
			     << " (" << (u64) floor(fileBytesRead/fileReads) << " bytes per)" << endl;
		}

	if (contains(debug,"reportrankselect"))
		{
		float rankAvg   = ((float) BitVector::totalRankCalls) / BitVector::totalRankNews;
		float selectAvg = ((float) BitVector::totalSelectCalls) / BitVector::totalSelectNews;

		cerr << "BV total rank() calls:   "
		     << BitVector::totalRankCalls   << "/" << BitVector::totalRankNews
		     << std::setprecision(1) << std::fixed << " (" << rankAvg << " avg)"
		     << endl;
		cerr << "BV total select() calls: "
		     << BitVector::totalSelectCalls << "/" << BitVector::totalSelectNews
		     << std::setprecision(1) << std::fixed << " (" << selectAvg << " avg)"
		     << endl;
		}

	if (reportTime)
		{
		double elapsedTime = elapsed_wall_time(startTime);
		cerr << "wallTime: " << elapsedTime << std::setprecision(6) << std::fixed << " secs" << endl;
		}

	if ((reportTime) || (contains(debug,"reporttotalloadtime")))
		{
		double totalLoadTime = BloomFilter::totalLoadTime + BitVector::totalLoadTime;
		cerr << "totalLoadTime: " << totalLoadTime << std::setprecision(6) << std::fixed << " secs" << endl;
		}

	return EXIT_SUCCESS;
	}

//----------
//
// read_queries--
//	Read the query file(s), populating the queries list.
//
// $$$ this should warn the user if the queries have any name used more than
//     once for sequences that aren't the same
//----------

void QueryCommand::read_queries()
	{
	// if no query files are provided, read from stdin

	if (queryFilenames.empty())
		Query::read_query_file (cin, /*filename*/ "", generalQueryThreshold, queries, repartFileName, winFileName);

	// otherwise, read each query file

	else
		{
		int numQueryFiles = queryFilenames.size();
		for (int queryIx=0 ; queryIx<numQueryFiles ; queryIx++)
			{
			string filename = queryFilenames[queryIx];
			std::ifstream in (filename);
			if (not in)
				fatal ("error: failed to open \"" + filename + "\"");
			Query::read_query_file (in, filename, queryThresholds[queryIx], queries, repartFileName, winFileName);
			in.close();
			}
		}

	}

//----------
//
// sort_matches_by_kmer_counts--
//	Sort query matches by decreasing kmer hit counts.
//
//----------

void QueryCommand::sort_matches_by_kmer_counts (void)
	{
	for (auto& q : queries)
		{
		vector<pair<u64,string>> matches;
		int matchIx = 0;
		for (auto& name : q->matches)
			{
			u64 numPassed = q->matchesNumPassed[matchIx];
			// (numPassed is negated sort will give decreasing order)
			matches.emplace_back(-(numPassed+1),name);
			matchIx++;
			}

		sort(matches.begin(),matches.end());

		matchIx = 0;
		for (const auto& matchPair : matches)
			{
			u64    negNumPassed = matchPair.first;
			string name         = matchPair.second;

			q->matches         [matchIx] = name;
			q->matchesNumPassed[matchIx] = (-negNumPassed) - 1;
			matchIx++;
			}
		}
		
	}

//----------
//
// print_matches--
//
//----------

void QueryCommand::print_matches
   (std::ostream& out) const
	{
	for (auto& q : queries)
		{
		out << "*" << q->name << " " << q->matches.size() << endl;
		if (reportNodesExamined)
			out << "# " << q->nodesExamined << " nodes examined" << endl;
		for (auto& name : q->matches)
			out << name << endl;
		}
	}

//----------
//
// print_matches_with_kmer_counts--
//
//----------

void QueryCommand::print_matches_with_kmer_counts
   (std::ostream& out) const
	{
	std::ios::fmtflags saveOutFlags(out.flags());

	for (auto& q : queries)
		{
			
		out << "*" << q->name << " " << q->matches.size() << endl;
		if (reportNodesExamined)
			out << "# " << q->nodesExamined << " nodes examined" << endl;
			

		int matchIx = 0;
		for (auto& name : q->matches)
			{
			u64 numPassed = q->matchesNumPassed[matchIx];

			

			out << name
			    << " " << numPassed << "/" << q->numPositions;
			if (q->numPositions == 0)
				out << " 0"; // instead of dividing by zero
			else
				out << " " << std::setprecision(6) << std::fixed << (numPassed/float(q->numPositions));

			out << endl;
			matchIx++;
			}
		}

	out.flags(saveOutFlags);
	}

//----------
//
// print_kmer_hit_counts--
//
//----------

void QueryCommand::print_kmer_hit_counts
   (std::ostream& out) const
	{
	std::ios::fmtflags saveOutFlags(out.flags());

	for (auto& q : queries)
		{
		int matchCount = 0;
		for (size_t matchIx=0 ; matchIx<q->matches.size() ; matchIx++)
			{
			u64 numPassed = q->matchesNumPassed[matchIx];
			bool queryPasses = (numPassed >= q->neededToPass);
			if (queryPasses) matchCount++;
			}

		out << "*" << q->name << " " << matchCount << endl;

		int matchIx = 0;
		for (auto& name : q->matches)
			{
			u64 numPassed = q->matchesNumPassed[matchIx];
			bool queryPasses = (numPassed >= q->neededToPass);

			out << q->name << " vs " << name
				<< " " << numPassed << "/" << q->numPositions;
			if (q->numPositions == 0)
				out << " 0"; // instead of dividing by zero
			else
				out << " " << std::setprecision(6) << std::fixed << (numPassed/float(q->numPositions));
			if (queryPasses) out << " hit";
			out << endl;
			matchIx++;
			}
		}

	out.flags(saveOutFlags);
	}
