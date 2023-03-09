
/*
 *   Load a file of data such and do a regexp matching with hyperscan
 *
 *   Copyright (C) 2023  Gerard Wagener
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Affero General Public License as
 *   published by the Free Software Foundation, either version 3 of the
 *   License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Affero General Public License for more details.
 *
 *   You should have received a copy of the GNU Affero General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <hs.h>
#include <string.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
using namespace std;


class kwmatch
{
    public:
        // Command line arguments
        string file;
        string name;
        string original;
        void usage(void);
        void load_regexp_file(void);
        void prepare(void);
        void process(void);

        //Buffer to copy the match in static function needs to access it
        char* buffer;
        size_t buffer_size = 4096;
        size_t match_buffer_size = 1024;
        char* match;
        size_t match_idx = 0;
        size_t match_size = 0;
private:
        vector <string> regexps;
        vector <string> names;
        vector <unsigned> flags;
        vector <unsigned> ids;

        // Hyperscan stuff
        hs_database_t* db;
        hs_scratch_t *scratch;
        hs_stream_t *stream;
        //TODO Some metrics not sure if everything is processed
   };

class kwmatchException
    {
    public:
        kwmatchException(kwmatch* kw, string errorMessage);

    private:
        string ErrorMessage;
        kwmatch* kw;

    friend ostream& operator<<(ostream& os, kwmatchException& kwe)
    {
        //TODO maybe return a timestamp too
        return os<<"[ERROR] "<<kwe.ErrorMessage<<".Command line arguments:"
        "file="<<kwe.kw->file<<",orginal="<<kwe.kw->original<<",name="<<kwe.kw->name<<endl;
    }
};


kwmatchException::kwmatchException(kwmatch* kwo, string errorMessage)
{
    this->ErrorMessage = errorMessage;
    // Store kwmatch object entirely so we have all variables in the Exception
    this->kw = kwo;
}

/* Problem: Variable to contains the offset where it matched however we do not
 * know how large the matched string was. Work around modify config file to add
 * a maximal string excpected per regexp?
 * Start of match is documented here
 * https://intel.github.io/hyperscan/dev-reference/compilation.html#som
 */
static
int onMatch(unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void *ctx) {
                     kwmatch *kw  = (kwmatch*)ctx;
    //Do not want to have any variables allocated here and reuse existing ones
    //cout << "[INFO] pattern match id=" <<id << ",from="<<from << ",to=" << to <<",falgs=" << flags<<endl;
    //FIXME go back until a space is found. Maybe kill all performance gains
    //FIXME check if there are more efficient implementations
    //FIXME also ignores matches going over multiple buffers
    kw->match_idx = 0;
    kw->match_size = 0;
    //cout << "BUFFER"<<kw->buffer<<"END BUFFER"<<endl;
    if ((to>0) and (to<kw->buffer_size)) {
        kw->match_idx = to -1;
        kw->match_size = 0;
        do {
            if ( kw->buffer[kw->match_idx] !=' ' and kw->buffer[kw->match_idx] != '\n') {
                //cout << "DEBUG: character ="<< kw->buffer[kw->match_idx] <<"kw->match_idx " <<kw->match_idx<<endl;
                kw->match[kw->match_size] = kw->buffer[kw->match_idx];
                //FIXME check boundaries
                kw->match_size++;
             } else {
                // Boundary found
                kw->match[kw->match_size]=0;
                break;
             }
             kw->match_idx--;
        } while (kw->match_idx > 0);
    }

    // Discard all matches smaller than 3 character
    // FIXME add this threshold as paramter
    if (kw->match_size < 4){
        return 0;
    }
    //FIXME String must be reversed. Problem I don't know what data I have,
    //unicode breaks, also to avoid plenty of copies of the string
    for (kw->match_idx = 0; kw->match_idx < kw->match_size / 2; kw->match_idx++){
        swap(kw->match[kw->match_idx], kw->match[kw->match_size - kw->match_idx - 1]);
    }
    //TODO filter out non printable characters
    cout <<"MATCH:"<<id<<":"<<kw->match<<endl;
    return 0; // continue matching
}




/*
 * Do all the preparation work for hyperscan
 */
//TODO raise exceptions
void kwmatch::prepare(void)
{
    hs_error_t err;
    hs_compile_error_t *compile_err = NULL;
    vector<const char*> cstrPatterns;

    this->load_regexp_file();
    this->db = NULL;

    // Conversion of patterns taken from
    // https://github.com/intel/hyperscan/blob/master/examples/pcapscan.cc
    for (const auto &pattern : this->regexps) {
        cstrPatterns.push_back(pattern.c_str());
    }

    for (const char* cstr: cstrPatterns){
        //cout << "[DEBUG] cstring regexp going to hs_compile_multi:" << cstr <<endl;
    }

    err = hs_compile_multi(cstrPatterns.data(), this->flags.data(), this->ids.data(), this->ids.size(), HS_MODE_STREAM, NULL, &(this->db), &compile_err);

    if (err != HS_SUCCESS) {
        cerr << "Could not compile regexp. Cause=" << compile_err->message << endl;
    }
    this->scratch = NULL;
    err = hs_alloc_scratch(db, &this->scratch);
    if (err != HS_SUCCESS) {
        cerr<< "[ERROR] Could not allocate scratch space. Error=" << err <<  endl;
    }

    this->stream = NULL;
    err = hs_open_stream(db, 0, &(this->stream));
    if (err != HS_SUCCESS) {
           cerr<< "ERROR: cannot open stream" << endl;
    }
}

/*
 * Load regular expressions from file with the format
 * id flag name regexp
 *
 * Returns true on success, false on failure
 */

void kwmatch::load_regexp_file(void)
{
    fstream fp;
    int id;
    int flag;
    string regexp;
    string line;
    string name;
    int i,j;
// Expect 4 offsets first for "id" "flag", "name" all the rest goes in regexp
    int idx[3]={0,0,0};

    fp.open(this->file);
    if (fp.is_open()) {
        while (getline(fp,line)) {
            if (line.rfind("#") == string::npos) {
                //regexp could have spaces aswell all the rest goes there
                j = 0;
                for (i=0;i<line.length();i++) {
                    if (line[i] == ' '){
                        if (j < 3) {
                            idx[j]=i;
                            j++;
                        }
                    }
                }
                //cout <<line<<" "<<idx[0]<<" "<<idx[1]<<" "<<idx[2]<<" " <<idx[3]<< endl;
                id = std::stoi(line.substr(0, idx[0]).c_str());
                flag = std::stoi(line.substr(idx[0]+1, idx[1]).c_str());
                name = line.substr(idx[1]+1,idx[2]-idx[1]-1);
                regexp = line.substr(idx[2]+1,string::npos);
                //Store only items where all fields are set to avoid segfaults
                //FIXME flags are ids are not checked
                if (!(name.empty()) and !(regexp.empty())){
                    this->ids.push_back(id);
                    this->flags.push_back(flag);
                    this->names.push_back(name);
                    this->regexps.push_back(regexp);
                    //cout<<"id="<<id<<",flag="<<flag<<",name="<<name<<",regexp="<<regexp <<endl;
                }
            }
       }
       fp.close();
       return;
    } else {
        throw kwmatchException(this,"Cannot open regular expression file.");
    }
 }

void kwmatch::usage(void)
{
    cout << "kwmatch - matches a set of regex defined in a file <file> on stdin"<<endl;
    cout << "kwmatch [-h] [-f filename] [-n name] [-o original] "<< endl;
    cout << endl<< "OPTIONS" << endl<<endl;
    cout <<"-h    --help     Shows this screen"<<endl;
    cout <<"-f    --file     File containing the regular expressions" <<endl;
    cout <<"-n    --name     Name where stdin came from i.e. compressed filename of the shard or URL" <<endl;
    cout <<"-o    --original Original name of the data" <<endl;

    cout << endl << "FORMAT of the regexp file" << endl << endl;
    cout << "One line contains a regexp with spaces seprated" << endl;
    cout << "Spaces are not forseen in names of regexp but they can be in the regexp itself" << endl;
    cout << "The following fields are expected per line" << endl;
    cout << "Format id flag name regexp" <<endl;
}

/*
 * Read from standard input and apply regular expressions on buffer
 */

void kwmatch::process(void)
{
    hs_error_t err;

    this->prepare();

    this->buffer = (char*)calloc(this->buffer_size,1);
    this->match = (char*)calloc(this->match_buffer_size,1);
    if (this->match == NULL) {
        throw kwmatchException(this, "Cannot allocate memory for  maching buffer");
    }
    size_t nread = 0;
    if (this->buffer != NULL) {
        do {
            nread = read(STDIN_FILENO, this->buffer, this->buffer_size);
            //cerr<<"[DEBUG] Read "<< nread << " bytes" <<endl;
            if (nread > 0 and (nread < buffer_size)) {
                err = hs_scan_stream(this->stream, this->buffer, nread, 0, scratch, onMatch, this);
                if (err != HS_SUCCESS) {
                    cerr<<"[ERROR] hs_scan returned error="<<err<<endl;
                }
            }
        } while (nread > 0);
    } else {
        cerr << "[ERROR] cannot allocate buffer. Requested size="<<this->buffer_size<<endl;
    }
}

int main(int argc, char* argv[])
{
    kwmatch kw;
    int next_option = 0;
    const char* const short_options = "hf:n:t:o";
    const struct option long_options[] = {
                { "help", 0, NULL, 'h' },
                { "file", 1, NULL, 'f' },
                { "name", 1, NULL, 'n' },
                { "orginal", 1, NULL, 'o'},
                {NULL,0,NULL,0}
   };

 do {
        next_option = getopt_long (argc, argv, short_options,
                                   long_options, NULL);
        if (next_option > 0) {
            switch(next_option)
            {
            case 'h':
                kw.usage();
                return EXIT_SUCCESS;
            case 'f':
                kw.file = string(optarg);
                break;
            case 'n':
                kw.name = string(optarg);
                break;
            case 'o':
                kw.original = string(optarg);
                break;
            default:
                /* Something unexpected happended */
                return EXIT_FAILURE;
            }
        }
 } while(next_option != -1);

    if (!kw.file.empty()){
        try {
            kw.process();
        } catch (kwmatchException kwe) {
            cout << kwe;
        }
   }
	return EXIT_SUCCESS;
}
