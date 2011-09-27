static char rcsid[] = "$Id: genome-write.c,v 1.6 2005/05/09 22:28:39 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "genome-write.h"

#ifdef WORDS_BIGENDIAN
#include "bigendian.h"
#else
#include "littleendian.h"
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "types.h"
#include "interval.h"
#include "compress.h"
#include "iit-write.h"
#include "complement.h"

#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif


/* Compressed genome file format: 32 characters in genome => 3
   bits/character = 96 bits, or 12 bytes, or 3 unsigned ints.  The
   first unsigned int (32 bits) has the high bits for these 32
   characters, the second unsigned int has the low bits, and the third
   unsigned int has the flags */

static void
find_positions (bool *revcompp, Genomicpos_T *leftposition, Genomicpos_T *rightposition,
		Genomicpos_T *startposition, Genomicpos_T *endposition, Genomicpos_T *truelength,
		int *contigtype, char *accession, IIT_T contig_iit) {
  int index;
  Interval_T interval;
  char *annotation;

  if ((index = IIT_find_one(contig_iit,accession)) == -1) {
    fprintf(stderr,"Can't find accession %s in contig IIT file\n",
	    accession);
    exit(9);
  } else {	
    interval = IIT_interval(contig_iit,index);

    *leftposition = Interval_low(interval);
    *rightposition = Interval_high(interval);

    annotation = IIT_annotation(contig_iit,index);
    if (annotation[0] == '-') {
      *revcompp = true;
      *startposition = Interval_high(interval) + 1U;
      *endposition = Interval_low(interval) + 1U;
    } else {
      *revcompp = false;
      *startposition = Interval_low(interval);
      *endposition = Interval_high(interval);
    }

    *truelength = Interval_length(interval);
    *contigtype = Interval_type(interval);
    debug(printf("revcompp = %d, leftposition = %d, rightposition = %d, startposition = %d, endposition = %d\n",
		 *revcompp,*leftposition,*rightposition,*startposition,*endposition));
    return;
  }
}

static void
seek (FILE *fp, long int offset) {
  int rc;

  if (fseek(fp,offset,SEEK_SET) < 0) {
    perror("Error in gmapindex, seek");
    exit(9);
  }
  return;
}

static void
fill_x (FILE *fp, Genomicpos_T startpos, Genomicpos_T endpos, bool uncompressedp) {
  char X[1];
  Genomicpos_T i;

  if (uncompressedp == true) {
    if (fseek(fp,startpos,SEEK_SET) < 0) {
      perror("Error in gmapindex, fill_x");
      exit(9);
    }
    X[0] = 'X';
    for (i = startpos; i < endpos; i++) {
      fwrite(X,sizeof(char),1,fp);
    }
  } else {
    Compress_update_file(fp,NULL,startpos,endpos);
  }
  return;
}

static void
fill_x_memory (UINT4 *genomecomp, Genomicpos_T startpos, Genomicpos_T endpos) {
  Compress_update_memory(genomecomp,NULL,startpos,endpos);
  return;
}


#define BUFFERSIZE 8192

static char *
parse_accession (char *Header) {
  char *accession, *p;
  int strlength;

  p = &(Header[1]);		/* First character is '>' */
  while (*p != '\0' && *p != '\n' && !isspace(*p)) {
    p++;
  }
  *p = '\0';
  strlength = (p - &(Header[1]))/sizeof(char);
  accession = (char *) CALLOC(strlength+1,sizeof(char));
  strcpy(accession,&(Header[1]));

  return accession;
}

static void
make_complement_buffered (char *complement, char *sequence, unsigned int length) {
  char complCode[128] = COMPLEMENT;
  int i, j;

  /* complement = (char *) CALLOC(length+1,sizeof(char)); */
  for (i = length-1, j = 0; i >= 0; i--, j++) {
    complement[j] = complCode[sequence[i]];
  }
  complement[length] = '\0';
  return;
}

/* Puts reference genome into refgenome_fp (potentially compressed!),
   and puts alternate strain sequences into altstrain_iit. */
static void
genome_write_file (FILE *refgenome_fp, FILE *input, 
		   IIT_T contig_iit, IIT_T altstrain_iit, bool uncompressedp) {
  char Buffer[BUFFERSIZE], Complement[BUFFERSIZE], *segment;
  char *accession, *p;
  Genomicpos_T leftposition, rightposition, startposition, endposition, truelength, 
    maxposition = 0, currposition = 0, i;
  int altstrain_index, altstrain_offset, contigtype, rc;
  bool revcompp;

  while (fgets(Buffer,BUFFERSIZE,input) != NULL) {
    if (Buffer[0] == '>') {
      /* HEADER */
      accession = parse_accession(Buffer);
      find_positions(&revcompp,&leftposition,&rightposition,&startposition,&endposition,
		     &truelength,&contigtype,accession,contig_iit);
      if (revcompp == true) {
	fprintf(stderr,"Writing contig %s to universal coordinates %u..%u",
		accession,startposition,endposition);
      } else {
	fprintf(stderr,"Writing contig %s to universal coordinates %u..%u",
		accession,startposition+1U,endposition+1U);
      }
      if (contigtype > 0) {
	fprintf(stderr," (alternate strain %s)",IIT_typestring(altstrain_iit,contigtype));
      }
      fprintf(stderr,"\n");
      FREE(accession);

      if (contigtype > 0) {
	/* Initialize file pointer for alternate strain */
	altstrain_index = IIT_get_exact(altstrain_iit,leftposition,rightposition,contigtype);
	if (revcompp == true) {
	  altstrain_offset = rightposition + 1U - leftposition;
	} else {
	  altstrain_offset = 0;
	}
	debug(printf("Setting altstrain_offset to be %d\n",altstrain_offset));
      }

      /* Handles case where true length is greater than provided
         coordinates.  This needs to be after call to IIT_get_exact */
      if (leftposition + truelength - 1U > rightposition) {
	debug(fprintf(stderr,"Extending endposition for truelength of %u\n",truelength));
	rightposition = leftposition + truelength;
	if (revcompp == true) {
	  endposition = startposition - truelength;
	} else {
	  endposition = startposition + truelength;
	}
      }

      /* In both cases, set file pointer for reference strain,
         although we won't write sequence of alternate strain.  For an
         alternate strain, ensure that we fill the reference strain
         with sufficient X's. */
      if (startposition > maxposition) {
	/* Start beyond end of file */
	debug(printf("Filling with X's from %u to %u-1\n",maxposition,startposition));
	fill_x(refgenome_fp,maxposition,startposition,uncompressedp);
	  
	if (contigtype > 0) {
	  fill_x(refgenome_fp,leftposition,rightposition + 1,uncompressedp);
	  maxposition = currposition = rightposition + 1;
	} else {
	  maxposition = rightposition;
	  currposition = startposition;
	}

      } else {
	/* Start within file */
	if (contigtype > 0) {
	  if (rightposition + 1 > maxposition) {
	    debug(printf("Filling with X's from %u to %u-1\n",maxposition,rightposition+1));
	    fill_x(refgenome_fp,maxposition,rightposition + 1,uncompressedp);
	    maxposition = currposition = rightposition + 1;
	  }
	} else {
	  debug(printf("Moving to %u\n",startposition));
	  if (uncompressedp == true) {
	    seek(refgenome_fp,startposition);
	  }
	  currposition = startposition;
	}
      }

    } else {
      /* SEQUENCE */
      if ((p = rindex(Buffer,'\n')) != NULL) {
	*p = '\0';
      }
      if (revcompp == true) {
	make_complement_buffered(Complement,Buffer,strlen(Buffer));
	segment = Complement;
      } else {
	segment = Buffer;
      }

      if (contigtype > 0) {
	/* Write alternate strain */
	if (revcompp == true) {
	  altstrain_offset -= strlen(segment);
	  debug(printf("Writing alternate strain at %u\n",altstrain_offset));
	  IIT_backfill_sequence(altstrain_iit,altstrain_index,altstrain_offset,segment);
	} else {
	  debug(printf("Writing alternate strain at %u\n",altstrain_offset));
	  IIT_backfill_sequence(altstrain_iit,altstrain_index,altstrain_offset,segment);
	  altstrain_offset += strlen(segment);
	}
      } else {
	/* Write reference strain */
	if (revcompp == true) {
	  debug(printf("Filling with sequence from %u-1 to %u\n",currposition,currposition-strlen(segment)));
	  currposition -= strlen(segment);
	  if (uncompressedp == true) {
	    fwrite(segment,sizeof(char),strlen(segment),refgenome_fp);
	  } else {
	    Compress_update_file(refgenome_fp,segment,currposition,currposition+strlen(segment));
	  }
	} else {
	  debug(printf("Filling with sequence from %u to %u-1\n",currposition,currposition+strlen(segment)));
	  if (uncompressedp == true) {
	    fwrite(segment,sizeof(char),strlen(segment),refgenome_fp);
	  } else {
	    Compress_update_file(refgenome_fp,segment,currposition,currposition+strlen(segment));
	  }
	  currposition += strlen(segment);
	  if (currposition > maxposition) {
	    maxposition = currposition;
	  }
	}
      }
    }
  }

  return;
}


/* Puts reference genome into refgenome_fp (assume compressed),
   and puts alternate strain sequences into altstrain_iit. */
static void
genome_write_memory (FILE *refgenome_fp, FILE *input, 
		     IIT_T contig_iit, IIT_T altstrain_iit, UINT4 *genomecomp,
		     unsigned int nuint4) {
  char Buffer[BUFFERSIZE], Complement[BUFFERSIZE], *segment;
  char *accession, *p;
  Genomicpos_T leftposition, rightposition, startposition, endposition, truelength, 
    maxposition = 0, currposition = 0, i;
  int altstrain_index, altstrain_offset, contigtype, rc;
  bool revcompp;

  while (fgets(Buffer,BUFFERSIZE,input) != NULL) {
    if (Buffer[0] == '>') {
      /* HEADER */
      accession = parse_accession(Buffer);
      find_positions(&revcompp,&leftposition,&rightposition,&startposition,&endposition,
		     &truelength,&contigtype,accession,contig_iit);
      if (revcompp == true) {
	fprintf(stderr,"Writing contig %s to universal coordinates %u..%u",
		accession,startposition,endposition);
      } else {
	fprintf(stderr,"Writing contig %s to universal coordinates %u..%u",
		accession,startposition+1U,endposition+1U);
      }
      if (contigtype > 0) {
	fprintf(stderr," (alternate strain %s)",IIT_typestring(altstrain_iit,contigtype));
      }
      fprintf(stderr,"\n");
      FREE(accession);

      if (contigtype > 0) {
	/* Initialize file pointer for alternate strain */
	altstrain_index = IIT_get_exact(altstrain_iit,leftposition,rightposition,contigtype);
	if (revcompp == true) {
	  altstrain_offset = rightposition + 1U - leftposition;
	} else {
	  altstrain_offset = 0;
	}
	debug(printf("Setting altstrain_offset to be %d\n",altstrain_offset));
      }

      /* Handles case where true length is greater than provided
         coordinates.  This needs to be after call to IIT_get_exact */
      if (leftposition + truelength - 1U > rightposition) {
	debug(fprintf(stderr,"Extending endposition for truelength of %u\n",truelength));
	rightposition = leftposition + truelength;
	if (revcompp == true) {
	  endposition = startposition - truelength;
	} else {
	  endposition = startposition + truelength;
	}
      }

      /* In both cases, set file pointer for reference strain,
         although we won't write sequence of alternate strain.  For an
         alternate strain, ensure that we fill the reference strain
         with sufficient X's. */
      if (startposition > maxposition) {
	/* Start beyond end of file */
	debug(printf("Filling with X's from %u to %u-1\n",maxposition,startposition));
	fill_x_memory(genomecomp,maxposition,startposition);
	  
	if (contigtype > 0) {
	  fill_x_memory(genomecomp,leftposition,rightposition + 1);
	  maxposition = currposition = rightposition + 1;
	} else {
	  maxposition = rightposition;
	  currposition = startposition;
	}

      } else {
	/* Start within file */
	if (contigtype > 0) {
	  if (rightposition + 1 > maxposition) {
	    debug(printf("Filling with X's from %u to %u-1\n",maxposition,rightposition+1));
	    fill_x_memory(genomecomp,maxposition,rightposition + 1);
	    maxposition = currposition = rightposition + 1;
	  }
	} else {
	  debug(printf("Moving to %u\n",startposition));
	  currposition = startposition;
	}
      }

    } else {
      /* SEQUENCE */
      if ((p = rindex(Buffer,'\n')) != NULL) {
	*p = '\0';
      }
      if (revcompp == true) {
	make_complement_buffered(Complement,Buffer,strlen(Buffer));
	segment = Complement;
      } else {
	segment = Buffer;
      }

      if (contigtype > 0) {
	/* Write alternate strain */
	if (revcompp == true) {
	  altstrain_offset -= strlen(segment);
	  debug(printf("Writing alternate strain at %u\n",altstrain_offset));
	  IIT_backfill_sequence(altstrain_iit,altstrain_index,altstrain_offset,segment);
	} else {
	  debug(printf("Writing alternate strain at %u\n",altstrain_offset));
	  IIT_backfill_sequence(altstrain_iit,altstrain_index,altstrain_offset,segment);
	  altstrain_offset += strlen(segment);
	}
      } else {
	/* Write reference strain */
	if (revcompp == true) {
	  debug(printf("Filling with sequence from %u-1 to %u\n",currposition,currposition-strlen(segment)));
	  currposition -= strlen(segment);
	  Compress_update_memory(genomecomp,segment,currposition,currposition+strlen(segment));
	} else {
	  debug(printf("Filling with sequence from %u to %u-1\n",currposition,currposition+strlen(segment)));
	  Compress_update_memory(genomecomp,segment,currposition,currposition+strlen(segment));
	  currposition += strlen(segment);
	  if (currposition > maxposition) {
	    maxposition = currposition;
	  }
	}
      }
    }
  }

  seek(refgenome_fp,0U);
  FWRITE_UINTS(genomecomp,nuint4,refgenome_fp);

  return;
}


void
Genome_write (char *genomesubdir, char *fileroot, FILE *input, 
	      IIT_T contig_iit, IIT_T altstrain_iit, bool uncompressedp,
	      bool writefilep, unsigned int genomelength) {
  unsigned int nuint4;
  FILE *refgenome_fp;
  char *filename;
  UINT4 *genomecomp;

  fprintf(stderr,"Genome length is %u nt\n",genomelength);
  if (uncompressedp == true) {
    filename = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			       strlen(fileroot)+strlen(".genome")+1,sizeof(char));
    sprintf(filename,"%s/%s.genome",genomesubdir,fileroot);
    if ((refgenome_fp = fopen(filename,"w")) == NULL) {
      fprintf(stderr,"Can't write to file %s\n",filename);
      exit(9);
    }
    genome_write_file(refgenome_fp,input,contig_iit,altstrain_iit,/*uncompressedp*/true);
    fclose(refgenome_fp);
    FREE(filename);

  } else if (writefilep == true) {
    filename = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			       strlen(fileroot)+strlen(".genomecomp")+1,sizeof(char));
    sprintf(filename,"%s/%s.genomecomp",genomesubdir,fileroot);
    fprintf(stderr,"User requested build of genome in file\n");

    if ((refgenome_fp = fopen(filename,"w+")) == NULL) {
      fprintf(stderr,"Can't open file %s for read/write\n",filename);
      exit(9);
    }
    genome_write_file(refgenome_fp,input,contig_iit,altstrain_iit,/*uncompressedp*/false);
    fclose(refgenome_fp);
    FREE(filename);

  } else {
    filename = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			       strlen(fileroot)+strlen(".genomecomp")+1,sizeof(char));
    sprintf(filename,"%s/%s.genomecomp",genomesubdir,fileroot);

    nuint4 = ((genomelength + 31)/32U)*3;
    fprintf(stderr,"Trying to allocate %d*%d bytes of memory...",nuint4,sizeof(UINT4));
    genomecomp = (UINT4 *) CALLOC_NO_EXCEPTION(nuint4,sizeof(UINT4));
    if (genomecomp == NULL) {
      fprintf(stderr,"failed.  Building genome in file.\n");
      if ((refgenome_fp = fopen(filename,"w+")) == NULL) {
	fprintf(stderr,"Can't open file %s for read/write\n",filename);
	exit(9);
      }
      genome_write_file(refgenome_fp,input,contig_iit,altstrain_iit,/*uncompressedp*/false);
      fclose(refgenome_fp);

    } else {
      fprintf(stderr,"succeeded.  Building genome in memory.\n");
      /* Creates X's at end */
      genomecomp[nuint4-3] = 0xFFFFFFFF;
      genomecomp[nuint4-2] = 0xFFFFFFFF;
      genomecomp[nuint4-1] = 0xFFFFFFFF;

      if ((refgenome_fp = fopen(filename,"w")) == NULL) {
	fprintf(stderr,"Can't open file %s for write\n",filename);
	exit(9);
      }
      genome_write_memory(refgenome_fp,input,contig_iit,altstrain_iit,genomecomp,nuint4);
      fclose(refgenome_fp);
      FREE(genomecomp);
    }

    FREE(filename);
  }

  return;
}



