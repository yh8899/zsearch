#ifndef INVERTED_INDEX_BATCH_H
#define INVERTED_INDEX_BATCH_H

#include <memory>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include <exception>

#include <atomic>
#include "port_posix.h"

#include <thread>

#include "IInvertedIndex.h"
#include "varint/Set.h"
#include "varint/CompressedSet.h"
#include "IKVStore.h"


#include "varint/ISetFactory.h"
#include "SparseSet.hpp"
#include <ctime>

struct postingComp {
  inline bool operator()(const std::pair<unsigned int,unsigned int>& first,
  const std::pair<unsigned int,unsigned int>& second) const{
   return (first.second < second.second);
  }
};

// The Goal of this file is to Sort and flush the list of (wordid,docid) pairs
// each time it reaches its maximum in-memory size using KVStore batching support
// but also avoid unserilizing a dccumenSet each time we add a new docID.
class InvertedIndexBatch : public IInvertedIndex
{
private:
	std::shared_ptr<KVStore::IKVStore> store;

	vector<std::pair<unsigned int,unsigned int>> postings;
	vector<std::pair<unsigned int,unsigned int>> postings2;

	atomic<vector<std::pair<unsigned int,unsigned int>>*> consumerVec;
	atomic<vector<std::pair<unsigned int,unsigned int>>*> producerVec;

	std::shared_ptr<ISetFactory> setFactory;

	int maxbatchsize;
	int minbatchsize;
	volatile int batchsize;

	// For the threading
	std::thread consumerThread;
    leveldb::port::Mutex m;
    leveldb::port::CondVar cond_var;
	volatile bool done = false;


	int storePut(unsigned int wordId, const shared_ptr<Set> set)
	{
		stringstream ss;
		set->write(ss);
		string bitmap = ss.str();

		if (store->Put(wordId,bitmap).ok())
		{
			return 1;
		}
		return 0;
	}

	int batchPut(unsigned int wordId, const shared_ptr<Set> set)
	{
		stringstream ss;
		set->write(ss);
		string bitmap = ss.str();
		store->PutBatch(wordId,bitmap);
		return 1;
	}


public:

	InvertedIndexBatch(std::shared_ptr<KVStore::IKVStore> store, shared_ptr<ISetFactory> setFactory) :
	 store(store),
	 setFactory(setFactory),
	 cond_var(&m)
	{
		void consumer_main();

		maxbatchsize = 18350080;
		minbatchsize =    28000;
		batchsize = 0;

		producerVec.store(&postings);
		consumerVec.store(&postings2);

		consumerThread = std::thread([this](){
			       this->consumer_main();
		        });

	}

	~InvertedIndexBatch() {
		flushBatch();
		stopConsumerThread();
	}

	void stopConsumerThread(){
		m.Lock();
		done=true;
		m.Unlock();
		//wakup if sleeping
		cond_var.Signal();
		//wait for termination
		consumerThread.join();
	}

	void shutDownBatchProcessor()
	{
		stopConsumerThread();
	}

	void setMaxBatchSize(unsigned int newSize)
	{
		maxbatchsize = newSize;
	}

    // we need an LRU cache her
	int get(unsigned int wordId, shared_ptr<Set>& inset) const
	{
		try
		{
			string bitmap;
			if(store->Get(wordId,bitmap).ok())
			{
				stringstream bitmapStream(bitmap);
				inset = setFactory->createSparseSet();
				inset->read(bitmapStream);
				return 1;
			}
		}
		catch (exception ex)
		{
			cerr << "get " << ex.what() << endl;
		}
		return 0;
	}

	bool exist(unsigned int wordId)
	{
		string ret;
		bool found = store->Get(wordId,ret).ok();
		return found;
	}

	shared_ptr<Set> getOrCreate(unsigned int wordid){
		shared_ptr<Set> docSet;
	  	if(!get(wordid,docSet)){
		    docSet = setFactory->createSparseSet();
		}
		return docSet;
	}


	// this is not good - if you call this from another thread that means that you'll be waiting for a while for this to finish
	void flushBatch(){
	   m.Lock();
	   while (batchsize > minbatchsize){
	     cond_var.Wait();
	   }
	   m.Unlock();
	  
	}



	int flushInBackground()
	{
		vector<std::pair<unsigned int,unsigned int>>& vec = *consumerVec.load();
		if (vec.size() > 0){
			std::stable_sort(vec.begin(),vec.end(),postingComp());

			unsigned int wordid = vec[0].second;
			shared_ptr<Set> docSet = getOrCreate(wordid);
			unsigned int last = vec[0].first;
			for (auto posting : vec){
				if (posting.second != wordid){
					batchPut(wordid, docSet);
					docSet = getOrCreate(posting.second);
					wordid = posting.second;
					last = posting.first;
				}
				docSet->addDoc(posting.first);
				assert(posting.first >= last);
				last = posting.first;

			}
			batchPut(wordid, docSet);
			vec.clear();
		}
		store->writeBatch();
		store->ClearBatch();
		return 1;
	}

	void consumer_main(){
	    while (!done) {
			bool haveWork = false;
		//	const clock_t start = clock();
		    m.Lock();
		    if (batchsize > minbatchsize){
				vector<std::pair<unsigned int,unsigned int>>* temp;
				temp = producerVec.load();
				producerVec.store(consumerVec.load());
				consumerVec.store(temp);
				batchsize = 0;
				
				haveWork = true;
		    } else {
			   // sleep
			   while (batchsize <= minbatchsize && !done){
			     cond_var.Wait(); // no maxwait ?
			   }
		    }
	        m.Unlock();
	        if (haveWork){
		        // use a ThreadPoolExecutor to execute it
		        flushInBackground();
                m.Lock();
                cond_var.Signal();
                m.Unlock();
	        }
			
	    }
	}

	int add(unsigned int wordId, unsigned int docid)
	{
		m.Lock();
		producerVec.load()->push_back(std::pair<unsigned int,unsigned int>(docid,wordId));
		batchsize +=1;
		cond_var.Signal();
		m.Unlock();
		if (batchsize > maxbatchsize){
		    flushBatch();
		}
		return 1;
	}



	void add(unsigned int docid, const SparseSet& documentWordId)
	{
		m.Lock();
		for (auto value : documentWordId)
		{
			producerVec.load()->push_back(std::pair<unsigned int, unsigned int>(docid, value));
			batchsize +=1;
		}
		if (batchsize > minbatchsize)
		{
		    cond_var.Signal();
        }
		m.Unlock();
	}
	
	// better batch add that doesnt lock and unlock for each wordid
	void add(unsigned int docid, const set<unsigned int>& documentWordId)
	{
		m.Lock();
		for (auto value : documentWordId)
		{
			producerVec.load()->push_back(std::pair<unsigned int, unsigned int>(docid, value));
			batchsize +=1;
		}
		if (batchsize > minbatchsize)
		{
		    cond_var.Signal();
        }
		m.Unlock();
	}

	int remove(unsigned int wordId, unsigned int docId)
	{
		throw -21;
	}

	int Compact(){
		store->Compact();
		return 1;
	}

};

#endif
