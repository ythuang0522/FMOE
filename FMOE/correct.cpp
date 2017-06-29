//-----------------------------------------------
// Copyright 2009 Wellcome Trust Sanger Institute
// Written by Jared Simpson (js18@sanger.ac.uk)
// Released under the GPL
//-----------------------------------------------
//
// correct - Correct sequencing errors in reads using the FM-index
//
#include <iostream>
#include <fstream>
#include <sstream>
#include <iterator>
#include "Util.h"
#include "correct.h"
#include "SuffixArray.h"
#include "BWT.h"
#include "SGACommon.h"
#include "OverlapCommon.h"
#include "Timer.h"
#include "BWTAlgorithms.h"
#include "ASQG.h"
#include "gzstream.h"
#include "SequenceProcessFramework.h"
#include "ErrorCorrectProcess.h"
#include "CorrectionThresholds.h"
#include "KmerDistribution.h"
#include "BWTIntervalCache.h"
//#include "LRAlignment.h"

// Functions
int learnKmerParameters(const BWT* pBWT);

//
// Getopt
//
#define SUBPROGRAM "correct"
static const char *CORRECT_VERSION_MESSAGE =
SUBPROGRAM " Version " PACKAGE_VERSION "\n"
"Written by Jared Simpson and revised by Yao-Ting Huang.\n"
"\n"
"Copyright 2010 Wellcome Trust Sanger Institute\n"
"Copyright 2014 National Chung Cheng Univresity\n";

static const char *CORRECT_USAGE_MESSAGE =
"Usage: " PACKAGE_NAME " " SUBPROGRAM " [OPTION] ... READSFILE\n"
"Correct sequencing errors in all the reads in READSFILE\n"
"\n"
"      --help                           display this help and exit\n"
"      -v, --verbose                    display verbose output\n"
"      -p, --prefix=PREFIX              use PREFIX for the names of the index files (default: prefix of the input file)\n"
"      -o, --outfile=FILE               write the corrected reads to FILE (default: READSFILE.ec.fa)\n"
"      -t, --threads=NUM                use NUM threads for the computation (default: 1)\n"
"      -a, --algorithm=STR              specify the correction algorithm to use. STR must be one of kmer, hybrid, overlap. (default: kmer)\n"
"          --metrics=FILE               collect error correction metrics (error rate by position in read, etc) and write them to FILE\n"
"\nKmer correction parameters:\n"
"      -K, --kmer-size=N                The length of the kmer to use. (default: 31)\n"
"      -k, --check-kmer-size=N          The length of the check kmer to use. (default: 7)\n"
"      -x, --kmer-threshold=N           Attempt to correct kmers that are seen less than N times. (default: 3)\n"
//"\nOverlap correction parameters:\n"
"      -e, --error-rate                 the maximum error rate allowed between two sequences to consider them overlapped (default: 0.04)\n"
"      -m, --min-overlap=LEN            minimum overlap required between two reads (default: 45)\n"
"\nReport bugs to " PACKAGE_BUGREPORT "\n\n";

static const char* PROGRAM_IDENT =
PACKAGE_NAME "::" SUBPROGRAM;

namespace opt
{
    static unsigned int verbose;
    static int numThreads = 1;
    static int numOverlapRounds = 1;
    static std::string prefix;
    static std::string readsFile;
    static std::string outFile;
    static std::string discardFile;
    static std::string metricsFile;
    static int sampleRate = BWT::DEFAULT_SAMPLE_RATE_SMALL;
    static std::string peReadsFile;

    static double errorRate = 0.04;
    static unsigned int minOverlap = DEFAULT_MIN_OVERLAP;
    static int seedLength = 0;
    static int seedStride = 0;
    static int conflictCutoff = 3;
    static int branchCutoff = -1;

    static int kmerLength = 31;
	static int check_kmerLength =7;
    static int kmerThreshold = 3;
    static int numKmerRounds = 10;
    static bool bLearnKmerParams = false;
	static bool diploid = false;

    static ErrorCorrectAlgorithm algorithm = ECA_FMEXTEND;
}

static const char* shortopts = "p:m:d:e:t:l:s:o:r:b:a:c:k:K:x:i:v";

enum { OPT_HELP = 1, OPT_VERSION, OPT_METRICS, OPT_DISCARD, OPT_LEARN, OPT_DIPLOID };

static const struct option longopts[] = {
    { "verbose",       no_argument,       NULL, 'v' },
    { "threads",       required_argument, NULL, 't' },
    { "min-overlap",   required_argument, NULL, 'm' },
    { "rounds",        required_argument, NULL, 'r' },
    { "outfile",       required_argument, NULL, 'o' },
    { "prefix",        required_argument, NULL, 'p' },
    { "error-rate",    required_argument, NULL, 'e' },
    { "seed-length",   required_argument, NULL, 'l' },
    { "seed-stride",   required_argument, NULL, 's' },
    { "algorithm",     required_argument, NULL, 'a' },
    { "sample-rate",   required_argument, NULL, 'd' },
    { "conflict",      required_argument, NULL, 'c' },
    { "branch-cutoff", required_argument, NULL, 'b' },
    { "kmer-size",     required_argument, NULL, 'k' },
	{ "check-kmer-size",     required_argument, NULL, 'K' },
    { "kmer-threshold",required_argument, NULL, 'x' },
    { "kmer-rounds",   required_argument, NULL, 'i' },
    { "learn",         no_argument,       NULL, OPT_LEARN },
    { "discard",       no_argument,       NULL, OPT_DISCARD },
    { "help",          no_argument,       NULL, OPT_HELP },
    { "version",       no_argument,       NULL, OPT_VERSION },
    { "metrics",       required_argument, NULL, OPT_METRICS },
	{ "diploid",       no_argument, NULL, OPT_DIPLOID },
    { NULL, 0, NULL, 0 }
};

//
// Main
//
int correctMain(int argc, char** argv)
{
    parseCorrectOptions(argc, argv);

    std::cout << "Correcting sequencing errors for " << opt::readsFile << "\n";

    // Set the error correction parameters
    ErrorCorrectParameters ecParams;

    // Load indices
    std::cout << "Loading BWT: " << opt::prefix + BWT_EXT << " and " << opt::prefix + RBWT_EXT << std::endl
              << "Loading Sampled Suffix Array: " << opt::prefix + SAI_EXT << std::endl;

    BWT* pBWT = new BWT(opt::prefix + BWT_EXT, opt::sampleRate);
    BWT* pRBWT = new BWT(opt::prefix + RBWT_EXT, opt::sampleRate);;
    SampledSuffixArray* pSSA = NULL;
    if(opt::algorithm == ECA_OVERLAP || opt::algorithm == ECA_HYBRID||opt::algorithm ==ECA_FMEXTEND)
        pSSA = new SampledSuffixArray(opt::prefix + SAI_EXT, SSA_FT_SAI);

    BWTIndexSet indexSet;
    indexSet.pBWT = pBWT;
    indexSet.pRBWT = pRBWT;
    indexSet.pSSA = pSSA;

    ecParams.indices = indexSet;

    // Learn the parameters of the kmer corrector
    if(opt::bLearnKmerParams)
    {
        int threshold = learnKmerParameters(pBWT);
        if(threshold != -1)
            CorrectionThresholds::Instance().setBaseMinSupport(threshold);
    }
	
	size_t n_samples = 10000;
	KmerDistribution kmerDistribution;
    int k = opt::kmerLength;
    for(size_t i = 0; i < n_samples; ++i)
    {
        std::string s = BWTAlgorithms::sampleRandomString(pBWT);
        int n = s.size();
        int nk = n - k + 1;
        for(int j = 0; j < nk; ++j)
        {
            std::string kmer = s.substr(j, k);
            int count = BWTAlgorithms::countSequenceOccurrences(kmer, pBWT);
            kmerDistribution.add(count);
        }
    }
	kmerDistribution.computeKDAttributes();
	//printf("The kmer median = %d\n",(int)kmerDistribution.getMedian());
	ecParams.solid_threshold=(int)kmerDistribution.getMedian();

    // Open outfiles and start a timer
    std::ostream* pWriter = createWriter(opt::outFile);
    std::ostream* pDiscardWriter = (!opt::discardFile.empty() ? createWriter(opt::discardFile) : NULL);
    Timer* pTimer = new Timer(PROGRAM_IDENT);

    ecParams.pOverlapper = NULL;
    ecParams.algorithm = opt::algorithm;

    ecParams.minOverlap = opt::minOverlap;
    ecParams.numOverlapRounds = opt::numOverlapRounds;
    ecParams.minIdentity = 1.0f - opt::errorRate;
    ecParams.conflictCutoff = opt::conflictCutoff;

    ecParams.numKmerRounds = opt::numKmerRounds;
    ecParams.kmerLength = opt::kmerLength;
	ecParams.check_kmerLength = opt::check_kmerLength;
    ecParams.printOverlaps = opt::verbose > 0;
	ecParams.isDiploid = opt::diploid;

    std::cout <<"Perform error correction using" << std::endl
              <<"kmer size=" << ecParams.kmerLength << std::endl
			  <<"Check kmer size=" << ecParams.check_kmerLength << std::endl
              <<"kmer threshold=" << opt::kmerThreshold <<std::endl
			  <<"overlap rounds=" << opt::numOverlapRounds <<std::endl;
    // Setup post-processor
    bool bCollectMetrics = !opt::metricsFile.empty();
    ErrorCorrectPostProcess postProcessor(pWriter, pDiscardWriter, bCollectMetrics);

    if(opt::numThreads <= 1)
    {
        // Serial mode
        ErrorCorrectProcess processor(ecParams);
        SequenceProcessFramework::processSequencesSerial<SequenceWorkItem,
                                                         ErrorCorrectResult,
                                                         ErrorCorrectProcess,
                                                         ErrorCorrectPostProcess>(opt::readsFile, &processor, &postProcessor);
    }
    else
    {
        // Parallel mode
        std::vector<ErrorCorrectProcess*> processorVector;
        for(int i = 0; i < opt::numThreads; ++i)
        {
            ErrorCorrectProcess* pProcessor = new ErrorCorrectProcess(ecParams);
            processorVector.push_back(pProcessor);
        }

        SequenceProcessFramework::processSequencesParallel<SequenceWorkItem,
                                                           ErrorCorrectResult,
                                                           ErrorCorrectProcess,
                                                           ErrorCorrectPostProcess>(opt::readsFile, processorVector, &postProcessor);

        for(int i = 0; i < opt::numThreads; ++i)
        {
            delete processorVector[i];
        }
    }

    if(bCollectMetrics)
    {
        std::ostream* pMetricsWriter = createWriter(opt::metricsFile);
        postProcessor.writeMetrics(pMetricsWriter);
        delete pMetricsWriter;
    }

    delete pBWT;
    //delete pIntervalCache;

    if(pRBWT != NULL)
        delete pRBWT;

    if(pSSA != NULL)
        delete pSSA;

    delete pTimer;

    delete pWriter;
    if(pDiscardWriter != NULL)
        delete pDiscardWriter;
		
    return 0;
}

// Learn parameters of the kmer corrector
int learnKmerParameters(const BWT* pBWT)
{
    std::cout << "Learning kmer parameters\n";
    srand(time(0));
    size_t n_samples = 10000;

    //
    KmerDistribution kmerDistribution;
    int k = opt::kmerLength;
    for(size_t i = 0; i < n_samples; ++i)
    {
        std::string s = BWTAlgorithms::sampleRandomString(pBWT);
        int n = s.size();
        int nk = n - k + 1;
        for(int j = 0; j < nk; ++j)
        {
            std::string kmer = s.substr(j, k);
            int count = BWTAlgorithms::countSequenceOccurrences(kmer, pBWT);
            kmerDistribution.add(count);
        }
    }

    //
    kmerDistribution.print(75);

    double ratio = 2.0f;
    int chosenThreshold = kmerDistribution.findErrorBoundaryByRatio(ratio);
    double cumulativeLEQ = kmerDistribution.getCumulativeProportionLEQ(chosenThreshold);

    if(chosenThreshold == -1)
    {
        std::cerr << "[sga correct] Error k-mer threshold learning failed\n";
        std::cerr << "[sga correct] This can indicate the k-mer you choose is too high or your data has very low coverage\n";
        exit(EXIT_FAILURE);
    }

    std::cout << "Chosen kmer threshold: " << chosenThreshold << "\n";
    std::cout << "Proportion of kmer density right of threshold: " << 1.0f - cumulativeLEQ << "\n";
    if(cumulativeLEQ > 0.25f)
    {
        std::cerr << "[sga correct] Warning: Proportion of kmers greater than the chosen threshold is less than 0.75 (" << 1.0f - cumulativeLEQ  << ")\n";
        std::cerr << "[sga correct] This can indicate your chosen kmer size is too large or your data is too low coverage to reliably correct\n";
        std::cerr << "[sga correct] It is suggest to lower the kmer size and/or choose the threshold manually\n";
    }

    return chosenThreshold;
}

//
// Handle command line arguments
//
void parseCorrectOptions(int argc, char** argv)
{
	optind=1;
    std::string algo_str;
    bool bDiscardReads = false;
    bool die = false;
    for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;)
    {
        std::istringstream arg(optarg != NULL ? optarg : "");
        switch (c)
        {
            case 'm': arg >> opt::minOverlap; break;
            case 'p': arg >> opt::prefix; break;
            case 'o': arg >> opt::outFile; break;
            case 'e': arg >> opt::errorRate; break;
            case 't': arg >> opt::numThreads; break;
            case 'l': arg >> opt::seedLength; break;
            case 's': arg >> opt::seedStride; break;
            case 'r': arg >> opt::numOverlapRounds; break;
            case 'a': arg >> algo_str; break;
            case 'd': arg >> opt::sampleRate; break;
            case 'c': arg >> opt::conflictCutoff; break;
            case 'k': arg >> opt::kmerLength; break;
			case 'K': arg >> opt::check_kmerLength; break;
            case 'x': arg >> opt::kmerThreshold; break;
            case '?': die = true; break;
            case 'v': opt::verbose++; break;
            case 'b': arg >> opt::branchCutoff; break;
            case 'i': arg >> opt::numKmerRounds; break;

            case OPT_LEARN: opt::bLearnKmerParams = true; break;
            case OPT_DISCARD: bDiscardReads = true; break;
            case OPT_METRICS: arg >> opt::metricsFile; break;
			case OPT_DIPLOID: opt::diploid = true; break;
            case OPT_HELP:
                std::cout << CORRECT_USAGE_MESSAGE;
                exit(EXIT_SUCCESS);
            case OPT_VERSION:
                std::cout << CORRECT_VERSION_MESSAGE;
                exit(EXIT_SUCCESS);
        }
    }

    if (argc - optind < 1)
    {
        std::cerr << SUBPROGRAM ": missing arguments\n";
        die = true;
    }
    else if (argc - optind > 1)
    {
        std::cerr << SUBPROGRAM ": too many arguments\n";
        die = true;
    }

    if(opt::numThreads <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid number of threads: " << opt::numThreads << "\n";
        die = true;
    }

    if(opt::numOverlapRounds <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid number of overlap rounds: " << opt::numOverlapRounds << ", must be at least 1\n";
        die = true;
    }

    if(opt::numKmerRounds <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid number of kmer rounds: " << opt::numKmerRounds << ", must be at least 1\n";
        die = true;
    }

    if(opt::kmerLength <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid kmer length: " << opt::kmerLength << ", must be greater than zero\n";
        die = true;
    }
	
	if(opt::check_kmerLength <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid check kmer length: " << opt::check_kmerLength << ", must be greater than zero\n";
        die = true;
    }

    if(opt::kmerThreshold <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid kmer threshold: " << opt::kmerThreshold << ", must be greater than zero\n";
        die = true;
    }

    // Determine the correction algorithm to use
    if(!algo_str.empty())
    {
        if(algo_str == "hybrid")
            opt::algorithm = ECA_HYBRID;
        else if(algo_str == "kmer")
            opt::algorithm = ECA_KMER;
        else if(algo_str == "overlap")
            opt::algorithm = ECA_OVERLAP;
		else if(algo_str == "fmextend")
            opt::algorithm = ECA_FMEXTEND;
		else
        {
            std::cerr << SUBPROGRAM << ": unrecognized -a,--algorithm parameter: " << algo_str << "\n";
            die = true;
        }
    }

    if (die)
    {
        std::cout << "\n" << CORRECT_USAGE_MESSAGE;
        exit(EXIT_FAILURE);
    }

    // Validate parameters
    if(opt::errorRate <= 0)
        opt::errorRate = 0.0f;

    if(opt::errorRate > 1.0f)
    {
        std::cerr << "Invalid error-rate parameter: " << opt::errorRate << "\n";
        exit(EXIT_FAILURE);
    }

    if(opt::seedLength < 0)
        opt::seedLength = 0;

    if(opt::seedLength > 0 && opt::seedStride <= 0)
        opt::seedStride = opt::seedLength;

    // Parse the input filenames
    opt::readsFile = argv[optind++];

    if(opt::prefix.empty())
    {
        opt::prefix = stripFilename(opt::readsFile);
    }

    // Set the correction threshold
    if(opt::kmerThreshold <= 0)
    {
        std::cerr << "Invalid kmer support threshold: " << opt::kmerThreshold << "\n";
        exit(EXIT_FAILURE);
    }
    CorrectionThresholds::Instance().setBaseMinSupport(opt::kmerThreshold);

    std::string out_prefix = stripFilename(opt::readsFile);
    if(opt::outFile.empty())
    {
        opt::outFile = out_prefix + ".ec.fa";
    }

    if(bDiscardReads)
    {
        opt::discardFile = out_prefix + ".discard.fa";
    }
    else
    {
        opt::discardFile.clear();
    }


}