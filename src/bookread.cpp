// Copyright 1993-1999, 2005, 2009, 2012-2014 by Jon Dart.
// All Rights Reserved.

#include "bookread.h"
#include "constant.h"
#include "bookdefs.h"
#include "movegen.h"
#include "globals.h"
#include "util.h"
#include "debug.h"
extern "C" {
#include <string.h>
#include <math.h>
};
#include <iostream> // for debugging
#include <assert.h>
#ifdef _WIN32
  #include <windows.h>
#endif

BookReader::BookReader(const char* fileName, const char* mappingName,
 bool create)
: book_file(NULL),
pBook(NULL),
pPage(NULL),
hFileMapping(NULL),
open(0),
current_page(-1)
{                 
   string book_path = derivePath(fileName);
   book_file = bu.openFile(book_path.c_str(),false);
   if (book_file == NULL) {
      cerr << "Warning: could not open book file" << endl;
      return;
   }
   if (book_file != NULL) {
      hFileMapping = bu.createMap(book_file,false);
      if (hFileMapping == NULL) {
#ifdef _WIN32
         ::MessageBox(NULL,"Error: could not create file mapping for opening book","Error",MB_OK | MB_ICONEXCLAMATION);
#else
	 cerr << "warning: file mapping failed for opening book" << endl;
#endif
      }
   }
   if (hFileMapping) {
      pBook = bu.map(hFileMapping,0,4096,false);
      if (pBook == NULL) {
#ifdef _WIN32
         ::MessageBox(NULL,"Error: file mapping failed for opening book","Error",MB_OK | MB_ICONEXCLAMATION);
#else 
	 cerr << "warning: file mapping failed for opening book" << endl;
#endif
      }
   }

   open = pBook != NULL;
   if (!open)
      return;
   cout << "Using book file " << fileName << endl;

   char *p = (char*)pBook;
   hdr.version = (byte)(*p);
   hdr.num_pages = (byte)p[1];
   // book file is little endian, convert if needed;
   hdr.page_capacity = swapEndian32((const byte*)(p+2));
   hdr.hash_table_size = swapEndian32((const byte*)(p+6));

   if (hdr.version != (byte)BookVersion)
   {
      char msg[128];
      sprintf(msg,"Expected book version %d, got %d instead",
              (int)BookVersion,(int)hdr.version);
#ifdef _WIN32
      MessageBox(NULL,msg,"Warning",MB_OK | MB_ICONEXCLAMATION);
#else
      fprintf(stderr,"Warning: %s\n",msg);
#endif
      bu.unmap(pBook,hdr.page_capacity);
      bu.closeMap(hFileMapping);
      bu.closeFile(book_file);
      open = 0;
      return;
   }
   bu.unmap(pBook,4096);
   pBook = NULL;
}

BookReader::~BookReader()
{
   if (pBook)
      bu.unmap(pBook,hdr.page_capacity);
   if (hFileMapping)
      bu.closeMap(hFileMapping);
   if (book_file)
      bu.closeFile(book_file);
}

void BookReader::syncCurrentPage() {
    bu.sync(pBook,hdr.page_capacity);
}

void BookReader::head( const Board & b, BookLocation &loc)
{
   loc.page = (int)(b.hashCode() % hdr.num_pages);
   loc.index = INVALID;
   // Don't return a book move if we have repeated this position
   // before .. make the program to search to see if the repetition
   // is desirable or not.
   if (b.repCount() > 0) {
     return;
   }
   fetch_page(loc.page);
   unsigned probe = (unsigned)((b.hashCode()>>32) % hdr.hash_table_size);

   // Copy the index from the book
   loc.index = swapEndian16(pPage+Header_Size+sizeof(uint16)*probe);
}

void BookReader::head( hash_t hashcode,
                       BookLocation &loc)
{
   loc.page = (int)(hashcode % hdr.num_pages);
   loc.index = INVALID;
   fetch_page(loc.page);
   unsigned probe = (unsigned)((hashcode>>32) % hdr.hash_table_size);

   // Copy the index from the book
   loc.index = swapEndian16(pPage+Header_Size+sizeof(uint16)*probe);
}

void BookReader::fetch( const BookLocation &loc, BookInfo &book_info )
{
   assert(pPage);

   byte *entry = pPage + Header_Size + hdr.hash_table_size*2 + ((int)loc.index)*Entry_Size;
   hash_t hc;

   // We assume here that the compiler doesn't reorder structures

   hc = swapEndian64(entry);
   book_info.my_hash_code = hc;
   book_info.learn_score = swapEndianFloat(entry+8);
   book_info.move_index = entry[12];
   book_info.learned_result = entry[13];
   book_info.weight = swapEndian16(entry+14);
   book_info.setLocation(loc);

}

void BookReader::update(const BookLocation &loc, float learn_factor)
{
   fetch_page(loc.page);
   byte *entry = pPage + Header_Size + hdr.hash_table_size*2 + ((int)loc.index)*Entry_Size;
   // get the existing learn factor
   float old_factor = swapEndianFloat(entry+8);
   // update it
   learn_factor += old_factor;
   // write to book image in memory
   float *e = (float*)(entry+8);
   *e = swapEndianFloat((byte*)&learn_factor);
   syncCurrentPage();
}

void BookReader::update(const BookLocation &loc,BookEntry *newEntry) {
   fetch_page(loc.page);
   byte *entry = pPage + Header_Size + hdr.hash_table_size*2 + ((int)loc.index)*Entry_Size;
   entry[13] = newEntry->learned_result;
   syncCurrentPage();
}


// Determine the weighting a book move will receive. Moves with higher weights
// will be played more often.
//
static int get_weight(const BookInfo &be) {
   int rec = be.getWeight();
#ifdef _TRACE
   cout << "index = " << (int)be.get_move_index() << " rec= " << rec << endl;
#endif
   if (rec == 0 && !options.book.random) {
      return 0;
   }
   
   // fold in result learning. 
   if (options.learning.result_learning) {
     int w2 = be.get_learned_result();
     if (w2 < 0) w2 = Util::Max(w2,-20);
     if (w2 > 0) w2 = Util::Min(w2,20);
     rec = rec*(100+w2)/100;
   }
   int base = Util::Min(BookEntry::MAX_WEIGHT,rec);
   if (base == 0) return 0;
   // Factor in score-based learning
   if (options.learning.score_learning) {
      int score = (int)be.learn_score;
      if (score >= 0)
         return Util::Min(BookEntry::MAX_WEIGHT,(base*(10+score))/10);
      else
         return Util::Max(0,(base*(10+score))/10);
   }
   else
     return base;
}

int BookReader::getBookMoves(const Board &b, const BookLocation &loc, BookEntry *moves, BookLocation *locs, int *scores, int limit) {
   //
   // Build a list of candidate moves.
   //
   BookEntry candidates[100];
   int candidate_weights[100];
   int candidate_count = 0; 

   BookLocation tmp = loc;
   while (tmp.index != INVALID) {
      BookInfo be;
      fetch(tmp, be);

      if (b.hashCode() == be.hashCode()) {
         int w = get_weight(be);
#ifdef _TRACE
         cout << "index = " << (int)tmp.index << " weight = " << w << " move index = " << (int)be.move_index << endl;
#endif
         if (w > 0) {
            candidate_weights[candidate_count] = w;
            candidates[candidate_count++] = be;
	 }
       }
       if (be.is_last())
          break;
       else
         tmp.index++;
   }
#ifdef _TRACE
   cout << "candidate_count = " << candidate_count << endl;
#endif

   // Sort by weights
   for (int i=1; i<candidate_count; i++) {
      int key = candidate_weights[i];
      BookInfo tmp = candidates[i];
      BookLocation loc = locs[i];
      int j = i-1;
      for (; j >= 0 && candidate_weights[j] < key; j--) {
         candidates[j+1] = candidates[j];
         candidate_weights[j+1] = candidate_weights[j];
         locs[j+1] = locs[j];
      }
      candidates[j+1] = tmp;
      candidate_weights[j+1] = key;
      locs[j+1] = loc;
   }

   int total_weight = 0;
   for (int i=0; i<candidate_count; i++) {
      if (i && candidate_weights[i] && options.book.selectivity < 50) {
         // At low selectivity settings, boost values of low ranked moves
         candidate_weights[i] = Util::Min(candidate_weights[0],
                                          candidate_weights[i]+
                                          candidate_weights[i]*(50-options.book.selectivity)/100);
      }
      total_weight += candidate_weights[i];
#ifdef _TRACE
      cout << " weight=" << candidate_weights[i] << endl;
#endif      
   }

#ifdef _TRACE
   cout << "selectivity=" << options.book.selectivity << endl;
#endif
   int candidate_count2 = 0;
   // Depending on the selectivity value, remove moves from the
   // candidate list.
   int tot = 0;
   // Stop selecting when accumulated move weight reaches "target". 
   // "target" is lower for higher selectivity values.
   int target = (int)(BookEntry::MAX_WEIGHT*(100.0-45.0*(2.0-log10(104-options.book.selectivity)))/100);
#ifdef _TRACE
   cout << "target weight=" << target << endl;
#endif
   for (int i = 0; i < candidate_count; i++) {
      int w  = candidate_weights[i];
      tot += w;
      scores[candidate_count2] = w;
      moves[candidate_count2++] = candidates[i];
      if (tot > target) {
         break;
      }
   }

   // renormalize after selectivity rules applied
   for (int i=0; i<candidate_count2; i++) {
      scores[i] = int((BookEntry::MAX_WEIGHT*scores[i])/total_weight + 0.5);
   }
   return candidate_count2;
}

Move BookReader::pick( const Board &b, const BookLocation &loc,
                        BookInfo &info )
{
#ifdef _TRACE
  cout << "BookReader::pick - hash=" << (hex) << b.hashCode() << (dec) << endl;
#endif
   BookLocation locs[100];
   BookEntry moves[100];
   int scores[100];
   int n = getBookMoves(b,loc,moves,locs,scores,100);
   return pickRandom(b,moves,scores,n,locs,info);
}



Move BookReader::pickRandom(const Board &b, BookEntry * candidates,int * candidate_weights,
  int candidate_count,BookLocation *locs,BookInfo &info)
{
   // If total_weight is 0, no moves have non-zero weights.
   int total_weight = 0;
   for (int i = 0; i < candidate_count; i++) total_weight += candidate_weights[i];
   if (total_weight == 0) return NullMove;
#ifdef _TRACE
   cout << "pickRandom: candidate count = " << candidate_count << " total_weight = " << total_weight << endl;
#endif   

   const unsigned nRand = rand() % total_weight;
   // Randomly pick from the available moves.  Prefer moves
   // with high weights.
#ifdef _TRACE
   cout << "nRand = " << nRand << endl; 
#endif
   unsigned weight = 0;
   for (int i=0; i < candidate_count; i++) 
   {
      int w = candidate_weights[i];
      weight += w;
      if (nRand <= weight)
      {
         // We have selected a move. The book contains
         // only the move index, not the move itself.
         // We must call the move generator to obtain the
         // move.
#ifdef _TRACE
        cout << "selecting " << (int) candidates[i].move_index << endl;
#endif
         Move moves[Constants::MaxMoves];
         MoveGenerator mg(b);
         int n = mg.generateAllMoves(moves,1 /*repeatable*/); 
#ifdef _TRACE
	 cout << " index=" << (int)candidates[i].move_index << 
	   " n=" << n << endl;
	 MoveImage(moves[candidates[i].move_index],cout); cout << endl;
#endif
         assert(candidates[i].move_index < n);
         // Copy selected book entry into info param - this is used
         // if we need to update the entry due to book learning.
         info.setMove(moves[candidates[i].move_index]);
         info.setTotalMoves(candidate_count);
         info.setLocation(locs[i]);
	 info.setHashCode(candidates[i].hashCode());
         return moves[candidates[i].move_index];
      }
   }
   // should never get here
   assert(0);
   return NullMove;
}

int BookReader::book_moves(const Board &b, Move *moves, int *scores, const unsigned limit)
{
   BookLocation loc;
   head(b,loc);
   BookEntry entries[100];
   BookLocation locs[100];
   int book_move_count = getBookMoves(b,loc,entries,locs,scores,100);

   Move move_list[Constants::MaxMoves];
   MoveGenerator mg( b, NULL, 0, NullMove, 0);
   (void)mg.generateAllMoves(move_list,1 /* repeatable */); 

   // convert move indexes to moves
   for (int i = 0; i < book_move_count; i++) {
      moves[i] = move_list[entries[i].move_index];
   }

   MoveGenerator::sortMoves(moves,scores,book_move_count);
   return book_move_count;
}

int BookReader::book_move_count(hash_t hashcode) {
   int num_moves = 0;
   BookLocation tmp;
   head(hashcode,tmp);
   BookEntry target(hashcode,0,0,0);
   BookLocation loc;
   head(hashcode,loc);
   tmp = loc;
   while (tmp.index != INVALID)
   {
      BookInfo be;
      fetch(tmp,be);
      int w;
             
      if (target == be && (w = get_weight(be)) != 0)
      {
	num_moves++;
      }
      if (be.is_last())
         break;
      else
         tmp.index++;
   }
   return num_moves;
}

void BookReader::fetch_page(int page)
{
   if (current_page != page)
   {
      if (pBook)
      {
         bu.unmap(pBook,hdr.page_capacity);
      }
      pBook = bu.map(hFileMapping,
                      page*hdr.page_capacity,hdr.page_capacity,false);
      if (!pBook)
      {
#ifdef _WIN32
         MessageBox( NULL, "File mapping failed for opening book", "Error", MB_ICONEXCLAMATION);
#endif
         pPage = NULL;
         return;
      }
      pPage = (byte*)pBook;
   }
   current_page = page;
}

