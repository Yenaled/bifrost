#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <stdint.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "boost/tuple/tuple.hpp"

#include "BloomFilter.hpp"
#include "Common.hpp"
#include "CompressedSequence.hpp"
#include "Contig.hpp"
#include "HashTables.hpp"
#include "Kmer.hpp"
#include "KmerIterator.hpp"
#include "KmerMapper.hpp"
#include "fastq.hpp"

using boost::tuples::tie;

pair<Kmer, size_t> find_contig_forward(BloomFilter &bf, Kmer km, string* s);
pair<ContigRef, pair<size_t, bool> > check_contig(BloomFilter &bf, KmerMapper &mapper, Kmer km);
string make_contig(BloomFilter &bf, KmerMapper &mapper, Kmer km);
void getMappingInfo(bool &repequal, int32_t &pos, size_t &dist, size_t &k, size_t &kmernum, int32_t &cmppos);

static const char alpha[4] = {'A','C','G','T'};
static const char beta[4] = {'T','G','A','C'}; // c -> beta[(c & 7) >> 1] maps: 'A' <-> 'T', 'C' <-> 'G'

struct BuildContigs_ProgramOptions {
  size_t k;
  size_t contig_size; // not configurable
  string input;
  string output;
  bool verbose;
  vector<string> files;
  BuildContigs_ProgramOptions() : k(0), verbose(false) , contig_size(1000000) {}
};


// use:  BuildContigs_PrintUsage();
// pre:   
// post: Information about how to "build contigs" has been inted to cerr
void BuildContigs_PrintUsage() {
  cerr << "BFGraph " << BFG_VERSION << endl << endl;
  cerr << "Filters errors in fastq or fasta files and saves results" << endl << endl;
  cerr << "Usage: BFGraph contigs [options] ... FASTQ files";
  cerr << endl << endl <<
      "-k, --kmer-size=INT             Size of k-mers, at most " << (int) (Kmer::MAX_K-1)<< endl << 
      "-i, --input=STRING            Filtered reads" << endl <<
      "-o, --output=STRING             Filename for output" << endl <<
      "    --verbose                   Print lots of messages during run" << endl << endl
      ;
}


// use:  BuildContigs_ParseOptions(argc, argv, opt);
// pre:  argc is the parameter count, argv is a list of valid parameters for 
//       "building contigs" and opt is ready to contain the parsed parameters
// post: All the parameters from argv have been parsed into opt
void BuildContigs_ParseOptions(int argc, char **argv, BuildContigs_ProgramOptions &opt) {
  int verbose_flag = 0;
  const char* opt_string = "k:o:i:";
  static struct option long_options[] =
      {
        {"verbose", no_argument,  &verbose_flag, 1},
        {"kmer-size", required_argument, 0, 'k'},
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {0,0,0,0}
      };

  int option_index = 0;
  int c;
  stringstream ss;
  stringstream ss2;
  while (true) {
    c = getopt_long(argc,argv,opt_string, long_options, &option_index);
    //cout << "debug: c="<<c<<", optarg="<<optarg << endl;
    if (c == -1) {
      break;
    }

    switch (c) {
      case 0: 
        break;
      case 'k': 
        opt.k = atoi(optarg); 
        break;
      case 'o': 
        opt.output = optarg;
        break;
      case 'i':
        opt.input = optarg;
        break;
      default: break;
    }
  }

  // all other arguments are fast[a/q] files to be read
  for (int i = optind; i < argc; ++i) {
    opt.files.push_back(argv[i]);
  }
  
  if (verbose_flag) {
    opt.verbose = true;
  }
}


// use:  b = BuildContigs_CheckOptions(opt);
// pre:  opt contains parameters for "building contigs"
// post: (b == true)  <==>  the parameters are valid
bool BuildContigs_CheckOptions(BuildContigs_ProgramOptions &opt) {
  bool ret = true;

  if (opt.k <= 0 || opt.k >= MAX_KMER_SIZE) {
    cerr << "Error, invalid value for kmer-size: " << opt.k << endl;
    cerr << "Values must be between 1 and " << (MAX_KMER_SIZE-1) << endl;
    ret = false;
  }

  
  if (opt.input.empty()) {
    cerr << "Input file missing" << endl;
  } else {
    struct stat inFileInfo;
    if (stat(opt.input.c_str(), &inFileInfo) != 0) {
      cerr << "Error input file not found " << opt.input << endl;
      ret = false;
    }
  }

  if (opt.files.size() == 0) {
    cerr << "Need to specify files for input" << endl;
    ret = false;
  } else {
    struct stat stFileInfo;
    vector<string>::const_iterator it;
    int intStat;
    for(it = opt.files.begin(); it != opt.files.end(); ++it) {
      intStat = stat(it->c_str(), &stFileInfo);
      if (intStat != 0) {
        cerr << "Error: file not found, " << *it << endl;
        ret = false;
      }
    }
  }
  
 

  //TODO: check if we have permission to write to outputfile
  
  return ret;

}


// use:  BuildContigs_PrintSummary(opt);
// pre:  opt has information about Kmer size, input file and output file
// post: Information about the Kmer size and the input and output files 
//       has been printed to cerr 
void BuildContigs_PrintSummary(const BuildContigs_ProgramOptions &opt) {
  cerr << "Kmer size" << opt.k << endl
       << "Reading input file " << opt.input << endl
       << "Writing to output " << opt.output << endl
       << "input files: " << endl;
  vector<string>::const_iterator it;
  for (it = opt.files.begin(); it != opt.files.end(); ++it) {
    cerr << "  " << *it << endl;
  }
}


// use:  BuildContigs_Normal(opt);
// pre:  opt has information about Kmer size, input file and output file
// post: The contigs have been written to the output file 
void BuildContigs_Normal(const BuildContigs_ProgramOptions &opt) {
  /**
   *  outline of algorithm
   *   - open bloom filter file
   *   - create contig datastructures
   *   - for each read
   *     - for all kmers in read
   *        - if kmer is in bf
   *          - if it maps to contig
   *            - try to jump over as many kmers as possible
   *          - else
   *            - create new contig from kmers in both directions
   *            - from this kmer while there is only one possible next kmer
   *            - with respect to the bloom filter
   *            - when the contig is ready, 
   *            - try to jump over as many kmers as possible
   */

  size_t num_threads = 1;
  #pragma omp parallel
  {
    #pragma omp master 
    {
      #ifdef _OPENMP
        num_threads = omp_get_num_threads(); 
      #endif 
    }
  }


  BloomFilter bf;
  FILE* f = fopen(opt.input.c_str(), "rb");
  if (f == NULL) {
    cerr << "Error, could not open file " << opt.input << endl;
    exit(1);
  } 

  if (!bf.ReadBloomFilter(f)) {
    cerr << "Error reading bloom filter from file " << opt.input << endl;
    fclose(f);
    f = NULL;
    exit(1);
  } else {
    fclose(f);
    f = NULL;
  }

  Kmer km, rep;
  KmerMapper mapper(opt.contig_size);

  KmerIterator iter, iterend;
  FastqFile FQ(opt.files);
  Contig *contig;
  ContigRef cr, mapcr;

  bool repequal, reversed;
  char name[8192], s[8192];
  size_t id, jumpi, kmernum, dist, name_len, len, k = Kmer::k;
  int32_t pos, cmppos, direction;
  uint64_t n_read = 0, num_kmers = 0, num_ins = 0;
  pair<size_t, bool> disteq;

  Kmer tmpkm;
  char kmrstr[200];

  vector<string> readv;
  vector<Kmer> *smallv, *parray = new vector<Kmer>[num_threads];
  bool done = false;
  size_t readindex, reads_now, read_chunksize = 100;

  cerr << "starting real work" << endl;
  while (!done) {
    readv.clear();
    reads_now = 0;

    while (reads_now < read_chunksize) {
      if (FQ.read_next(name, &name_len, s, &len, NULL, NULL) >= 0) {
        readv.push_back(string(s));
        ++n_read;
        ++reads_now;
      } else {
        done = true;
        break;
      }
    }

    #pragma omp parallel default(shared) private(dist,mapcr,disteq,kmernum,cmppos,smallv,repequal) shared(mapper,parray,readv,bf,reads_now,k)
    {
      KmerIterator iter, iterend;
      size_t threadnum = 0;
      #ifdef _OPENMP
        threadnum= omp_get_thread_num();
      #endif
      smallv = &parray[threadnum];


      #pragma omp for nowait
      for(int index=0; index < reads_now; ++index) {
        iter = KmerIterator(readv[index].c_str());
        Kmer km, rep;

        if (iter != iterend) {
          km = iter->first;
          rep = km.rep();
        }

        while (iter != iterend) {
          if (!bf.contains(rep)) { // km is not in the graph
            // jump over it
            iter.raise(km, rep);
          } else {
            tie(mapcr, disteq) = check_contig(bf, mapper, km);

            if (mapcr.isEmpty()) {
              // Make contig (or increase coverage) from this kmer after this thread finishes
              smallv->push_back(km);
              iter.raise(km, rep);
            } else {
              Contig *contig = mapper.getContig(mapcr).ref.contig;
              tie(dist, repequal) = disteq;
              int32_t pos = mapcr.ref.idpos.pos;

              getMappingInfo(repequal, pos, dist, k, kmernum, cmppos);        
              bool reversed = (pos >= 0) != repequal;
              size_t jumpi = 1 + iter->second + contig->seq.jump(readv[index].c_str(), iter->second + k, cmppos, reversed);

              if (reversed) {
                assert(contig->seq.getKmer(kmernum) == km.twin());
              } else {
                assert(contig->seq.getKmer(kmernum) == km);
              }
              #pragma omp critical 
              {
                if (contig->cov[kmernum] < 0xff) {
                  contig->cov[kmernum] += 1;
                }
              }
              int32_t direction = reversed ? -1 : 1;
              kmernum += direction;
              iter.raise(km, rep);

              while (iter != iterend && iter->second < jumpi) {
                assert(kmernum >= 0);
                assert(kmernum < contig->covlength);
                #pragma omp critical 
                {
                  if (contig->cov[kmernum] < 0xff) {
                    contig->cov[kmernum] += 1;
                  }
                }
                kmernum += direction;
                iter.raise(km, rep);
              }
            }
          }     
        }
      }
    }

    for (int i=0; i < num_threads; i++) {
      for (vector<Kmer>::iterator it=parray[i].begin(); it != parray[i].end(); ++it) {
        // The kmer did not map when it was added to this vector
        // so we make the contig if it has not been made yet
        km = *it;
        tie(mapcr, disteq) = check_contig(bf, mapper, km);
        
        if(mapcr.isEmpty()) {
          string seq = make_contig(bf, mapper, km);
          id = mapper.addContig(seq);
          //mapper.printContig(id);
          tie(mapcr, disteq) = check_contig(bf, mapper, km);
        } 

        // Increase coverage
        contig = mapper.getContig(mapcr).ref.contig;

        tie(dist, repequal) = disteq;
        pos = mapcr.ref.idpos.pos;

        getMappingInfo(repequal, pos, dist, k, kmernum, cmppos);        
        reversed = (pos >= 0) != repequal;

        if (reversed) {
          assert(contig->seq.getKmer(kmernum) == km.twin());
        }
        else {
          assert(contig->seq.getKmer(kmernum) == km);
        }
        if (contig->cov[kmernum] < 0xff) {
          contig->cov[kmernum] += 1;
        }
      }
      parray[i].clear();
    }
  }
  // Print the good contigs
  size_t contigsBefore = mapper.contigCount();
  int contigDiff = mapper.splitAndJoinContigs();
  int contigsAfter = contigsBefore + contigDiff;
  mapper.printContigs();
  cerr << "Before split and join: " << contigsBefore << " contigs" << endl;
  cerr << "After split and join: " << contigsAfter << " contigs" <<  endl;
  cerr << "Number of reads " << n_read  << ", kmers stored " << mapper.size() << endl;
  delete [] parray;
}


void getMappingInfo(bool &repequal, int32_t &pos, size_t &dist, size_t &k, size_t &kmernum, int32_t &cmppos) {
  // Now we find the right location of the kmer inside the contig
  // to increase coverage 
  if (pos >= 0) {
    if (repequal) {
      cmppos = pos - dist + k;
      kmernum = cmppos - k;
    } else {
      cmppos = pos - 1 + dist;
      kmernum = cmppos +1;
    }
  } else {
    if (repequal) {
      cmppos = -pos + dist -k;
      kmernum = cmppos +1;
    } else {
      cmppos = -pos + 1 - dist; // Original: (-pos +1 -k) - dist + k
      kmernum = cmppos - k;
    }
  }
}

// use:  BuildContigs(argc, argv);
// pre:  argc is the number of arguments in argv and argv includes 
//       arguments for "building the contigs", including filenames
// post: If the number of arguments is correct and the arguments are valid
//       the "contigs have been built" and written to a file
void BuildContigs(int argc, char** argv) {
  
  BuildContigs_ProgramOptions opt;

  BuildContigs_ParseOptions(argc,argv,opt);

  if (argc < 2) {
    BuildContigs_PrintUsage();
    exit(1);
  }
  
  if (!BuildContigs_CheckOptions(opt)) {
    BuildContigs_PrintUsage();
    exit(1);
  }
  
  // set static global k-value
  Kmer::set_k(opt.k);

  if (opt.verbose) {
    BuildContigs_PrintSummary(opt);
  }

  BuildContigs_Normal(opt);

}



// use:  (cr, (dist, eq)) = check_contig_(bf,km,mapper);
// pre:  
// post: if km does not map to a contig: cr.isEmpty() == true and dist == 0
//       else: km is in a contig which cr maps to and dist is the distance 
//             from km to the mapping location 
//             (eq == true):  km has the same direction as the contig
//             else:  km has the opposite direction to the contig
pair<ContigRef, pair<size_t, bool> > check_contig(BloomFilter &bf, KmerMapper &mapper, Kmer km) {
  ContigRef cr = mapper.find(km);
  if (!cr.isEmpty()) {
    return make_pair(cr, make_pair(0, km == km.rep())); 
  }
  int i, j;
  size_t dist = 1;
  bool found = false;
  Kmer end = km;
  while (dist < mapper.stride) {
    size_t fw_count = 0;
    j = -1;
    for (i = 0; i < 4; ++i) {
      Kmer fw_rep = end.forwardBase(alpha[i]).rep();
      if (bf.contains(fw_rep)) {
        j = i;
        ++fw_count;
        if (fw_count > 1) {
          break;
        }
      }
    }

    if (fw_count != 1) {
      break;
    }

    Kmer fw = end.forwardBase(alpha[j]);

    size_t bw_count = 0;
    for (i = 0; i < 4; ++i) {
      Kmer bw_rep = fw.backwardBase(alpha[i]).rep();
      if (bf.contains(bw_rep)) {
        ++bw_count;
        if (bw_count > 1) {
          break;
        }
      }
    }

    assert(bw_count >= 1);
    if (bw_count != 1) {
      break;
    }
    cr = mapper.find(fw);
    end = fw;
    if (!cr.isEmpty()) {
      found = true;
      break;
    }
    ++dist;
  }
  if (found) {
    return make_pair(cr, make_pair(dist, end == end.rep())); 
  } else {
    return make_pair(ContigRef(), make_pair(0,0));
  }
}

// use:  seq = make_contig(bf, mapper, km, s);
// pre:  km is not contained in a mapped contig in mapper 
// post: Finds the forward and backward limits of the contig
//       which contains km  according to the bloom filter bf and puts it into seq
string make_contig(BloomFilter &bf, KmerMapper &mapper, Kmer km) {
  char c, cc;
  size_t k  = Kmer::k;
  string seq, seq_fw(k, 0), seq_bw(k, 0);
  pair<Kmer, size_t> p_fw = find_contig_forward(bf, km, &seq_fw);
  pair<Kmer, size_t> p_bw = find_contig_forward(bf, km.twin(), &seq_bw);
  ContigRef cr_tw_end = mapper.find(p_bw.first);
  assert(cr_tw_end.isEmpty());

  if (p_bw.second > 1) {
    seq.reserve(seq_bw.size() + seq_fw.size() - k);
    // copy reverse part of seq_bw not including k
    for (int j = seq_bw.size() - 1; j >= k; --j) {
      seq.push_back(beta[(seq_bw[j] & 7) >> 1]);
    }
    // append seq_fw
    seq += seq_fw;
  } else {
    seq = seq_fw;
  }
  return seq;
}

// use:  tie(end,dist) = find_contig_forward(bf,km,s);
// pre:  
// post: km is contained in a contig c with respect to the
//       bloom filter graph bf and end is the forward endpoint (wrt km direction)
//       and c contains dist kmers until the end (including km)
//       if s is not NULL the sequence of the contig is stored in s
pair<Kmer, size_t> find_contig_forward(BloomFilter &bf, Kmer km, string* s) {
  int i, j;
  size_t dist = 1;
  vector<char> v;

  Kmer end = km;

  assert(bf.contains(km.rep()));
  if (s != NULL) {
    char t[Kmer::MAX_K+1];
    km.toString(t);
    for (i = 0; i < Kmer::k; ++i) {
      v.push_back(t[i]);
    }
  }
  
  while (true) {
    assert(bf.contains(end.rep()));
    size_t fw_count = 0;
    j = -1;
    for (i = 0; i < 4; ++i) {
      Kmer fw_rep = end.forwardBase(alpha[i]).rep();
      if (bf.contains(fw_rep)) {
        j = i;
        ++fw_count;
        if (fw_count > 1) {
          break;
        }
      }
    }

    if (fw_count != 1) {
      break;
    }
    
    Kmer fw = end.forwardBase(alpha[j]);
    assert(0 <= j && j < 4);
    assert(bf.contains(fw.rep()));

    size_t bw_count = 0;
    for (i = 0; i < 4; ++i) {
      Kmer bw_rep = fw.backwardBase(alpha[i]).rep();
      if (bf.contains(bw_rep)) {
        ++bw_count;
        if (bw_count > 1) {
          break;
        }
      }
    }

    assert(bw_count >= 1);
    if (bw_count != 1) {
      break;
    }
    
    end = fw;
    ++dist;
    if (s != NULL) {
      v.push_back(alpha[j]);
    }
  }

  if (s != NULL) {
    s->clear();
    s->reserve(v.size());
    s->insert(s->begin(), v.begin(), v.end());
  }
  return make_pair(end, dist);
}
