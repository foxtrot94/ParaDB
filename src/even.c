/*
 * even.c
 *
 */

#include "even.h"

//Main function for all even-numbered processes (including Root)
void pEven(DB_Context* handle){
	Query inputQuery;
	RowList queryResults;
	RowList querySorted;

	//Initialize your dynamic lists!
	RowList_init(&queryResults);
	RowList_init(&querySorted);

	while(1){
		//Root shows menu and asks input
		if(handle->rank==0){
			inputQuery = requestUserInput();
			if(inputQuery.type==EXIT){
				safeWrite("\nThank you for using ParaDB... Good Bye!");
			}
			else{
				printQueryInProcess();
			}
		}

		//We need to add a barrier to synchronize the rest of the processes (Do we?)
		MPI_Barrier(handle->coworkers); //Might not be necessary

		forwardQuery(handle,&inputQuery); //send it out
		if(inputQuery.type==EXIT){ break; } //Leave if needed...
		queryResults = waitForQueryReply(handle,&inputQuery); //... Or get it back

		querySorted = processQuery(handle,&inputQuery,queryResults); //TODO: enter this method and DO BUCKET SORT!

		//Root delivers!
		if(handle->rank==0){
			printQueryResults(inputQuery,querySorted.rows,querySorted.size);
		}


		//Recycle the used types
		//TODO: Think this one through a bit better. BucketSort will likely return new lists that need to be recycled
//		RowList_terminate(&queryResults);
//
//		if(handle->rank==0){
//
//			RowList_terminate(&querySorted);
//		}
		//TODO: Remove these leaky statements!
//		RowList_recycle(&queryResults);
//		RowList_recycle(&querySorted);
	}

	//cleanup the lists before returning to master!
	if(queryResults.size)
		RowList_terminate(&queryResults);
	if(querySorted.size && querySorted.rows!=queryResults.rows)
		RowList_terminate(&querySorted);
}

//Process the incoming query. Forwards it to the odd process and returns a dynamic list of the results.
RowList waitForQueryReply(DB_Context* handle, Query* aQuery){
	RowList retVal;
	DBRow* response;
	int responseSize;

	MPI_Status op_status;

	//Get the result back!
	//Probe first, get_count and calloc the buffer!
	MPI_Probe((handle->rank)+1,0,handle->all,&op_status);
	MPI_Get_count(&op_status,handle->row,&responseSize);
	response = calloc(responseSize,sizeof(DBRow));

	//Now we're ready for that message
	MPI_Recv(response,responseSize,handle->row,(handle->rank)+1,0,handle->all,&op_status);
	printf("even received %d\n",responseSize);

	//Make it into a high level format
	RowList_fit(&retVal,response,responseSize);

	return retVal;
}

//Only sends the Query through the system in the correct order.
void forwardQuery(DB_Context* handle, Query* aQuery){
	//Forward to other even nodes
	MPI_Bcast(aQuery,1,handle->query,0,handle->coworkers);

	//From there, give off the odd processes
	MPI_Send(aQuery,1,handle->query,(handle->rank)+1,0,handle->all);
}

RowList processQuery(DB_Context* handle,Query* aQuery, RowList partials){
	//For a sales by date query, we need to bucket sort
	if(aQuery->type==SALES_BY_DATE){
		//TODO: IMPLEMENT BUCKET SORT AND UNCOMMENT
		printf("Processing %d rows in query\n",partials.size);
		safeWrite("PROCESSING DATES!");
		return bucketSort(handle,aQuery,partials.rows,partials.size);
	}

	//This is simpler if we just reduce here.
	if(aQuery->type==SALES_BY_COMPANY){
		//here we'd want to simply reduce the results at the root
		//Just do successive MPI_Reduce operations to fill in the return array
		int maxRounds = partials.size; //corresponds to Max Company ID by now.
		DBRow* reduced;
		int i=0;

		if(handle->rank==0){
			reduced = calloc(maxRounds,sizeof(DBRow));
			//Make an exact copy of the significant info
			for(i=0;i<maxRounds;++i){
				reduced[i].company_id=i+1;
			}
		}

		//This for loop simply sums the results retrieved from all coworker nodes.
		for(i=0;i<maxRounds;++i){
			MPI_Reduce(&(partials.rows[i].sales_total),&(reduced[i].sales_total),1,MPI_FLOAT,MPI_SUM,0,handle->coworkers);
		}

		if(handle->rank==0){
			//If root, return a new container
			RowList container;
			RowList_fit(&container,reduced,maxRounds);
			return container;
		}
		else{
			//Other processes will get back what they came with
			return partials;
		}

	}
}

//Perform a full bucket sort operation such that the Root has the final values!
RowList bucketSort(DB_Context* handle, Query* aQuery, DBRow* partials, int partialsLength){
	//Ideally, we would make this function very general. The logic indeed is.
	// However, we'll just do a pass with Dates here and thus make it more specific to that
	//

	RowList retVal;
	int evenProcessCount;
	int expectedBufferSize=0;
	int* expectedSendSize;
	int* expectedReceiveSize;
	int* offsetsSend;
	int* offsetsReceive;
	DBRow* bucketed;
	int bucketedLength=0;
	DBRow* finals;
	int finalsLength=0;
	int i=0;

	//Get our coworkers size and make an array of said size
	MPI_Comm_size(handle->coworkers,&evenProcessCount);
	if(evenProcessCount==1){
		//you're done at this step if there's only a single process. No buckets to make
		qsort(partials,partialsLength,sizeof(DBRow),compareDates);
		RowList_fit(&retVal,partials,partialsLength);
		return retVal;
	}
	expectedSendSize = calloc(evenProcessCount,sizeof(int));
	expectedReceiveSize = calloc(evenProcessCount,sizeof(int));
	offsetsSend = calloc(evenProcessCount,sizeof(int));
	offsetsReceive = calloc(evenProcessCount,sizeof(int));


	//First, initially sort all of the elements
	qsort(partials,partialsLength,sizeof(DBRow),compareDates);



	//Now make the buckets.
	//Again, this will work only for dates. If we needed to extend this program, this would get delegated to its own function.
	Date startDate = aQuery->conditions.startDate;
	Date endDate = aQuery->conditions.endDate;
	long long startTime = convertDateToEpoch(&startDate);
	long long endTime = convertDateToEpoch(&endDate);

	getAllToAllParameters(startTime, endTime, evenProcessCount,partials,partialsLength,expectedSendSize,offsetsSend);

	printDivision(handle->rank,expectedSendSize,offsetsSend,evenProcessCount); 	//TODO: REMOVE Debug log

	//Send these parameters
	//Tell them the size...
	MPI_Alltoall(expectedSendSize,evenProcessCount,MPI_INTEGER,expectedReceiveSize,evenProcessCount,MPI_INTEGER,handle->coworkers);
	//...And expected offsets in advance
	MPI_Alltoall(offsetsSend,evenProcessCount,MPI_INTEGER,offsetsReceive,evenProcessCount,MPI_INTEGER,handle->coworkers);

	//This allows you to allocate accordingly
	for(i=0;i<evenProcessCount;++i){
		bucketedLength+=expectedReceiveSize[i];
		printf("%d. Expecting %d from P%d\n",handle->rank,expectedReceiveSize[i],i);
		fflush(stdout);
	}
	bucketed = calloc(bucketedLength,sizeof(DBRow));

	//Now, do your personalized all to all to send those partials to them buckets!
	//TODO: Check from here. Crashed at this call.
	MPI_Alltoallv(partials,expectedSendSize,offsetsSend,handle->row,bucketed,expectedReceiveSize,offsetsReceive,handle->row,handle->coworkers);

	//Sort your new stuff locally
	qsort(bucketed,bucketedLength,sizeof(DBRow),compareDates);


	printSalesByDate(bucketed, bucketedLength);

	//And finally prepare to Gather at root
	//Get the final bucket size for each node
	MPI_Gather(&bucketedLength,1,MPI_INT,expectedReceiveSize,evenProcessCount,MPI_INT,0,handle->coworkers);

	//Prepare the root node
	if(handle->rank==0){
		//Free the past buffer and ask for a new one
		free(offsetsReceive);
		offsetsReceive = NULL;
		offsetsReceive = calloc(evenProcessCount,sizeof(int));
		for(i=0;i<evenProcessCount;++i){
			//Get the running sum
			expectedBufferSize += expectedReceiveSize[i];
			printf("Expecting %d from P%d\n",expectedReceiveSize[i],i);
			fflush(stdout);
			if(i<evenProcessCount-1){ //And also calculate the displacements.
				offsetsReceive[i+1]=expectedReceiveSize[i]+offsetsReceive[i]; //TODO: Double check if that -1 causes any trouble or not :/
			}
		}
		finals = calloc(expectedBufferSize,sizeof(DBRow));
		safeWrite("Receiving query results!\n");
	}

	//Perform the final gather
	MPI_Gatherv(bucketed,bucketedLength,handle->row,finals,expectedReceiveSize,offsetsReceive,handle->row,0,handle->coworkers);

	if(handle->rank==0){
		RowList_fit(&retVal,finals,expectedBufferSize);
		free(bucketed);
	}
	else{
		RowList_fit(&retVal,bucketed,bucketedLength);
	}

	free(expectedSendSize);
	free(expectedReceiveSize);
	free(offsetsSend);
	free(offsetsReceive);

	return retVal;
}

void cleanupAfterQuery(DB_Context* handle, Query* aQuery, RowList partials, RowList finals){
	if(handle->rank==0){


	}
	else{
		RowList_terminate(&partials);
	}
}
