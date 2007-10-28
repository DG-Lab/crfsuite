/*
 *		Tag command for libCRF frontend.
 *
 *	Copyright (C) 2007 Naoaki Okazaki
 *
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

#include <os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <crf.h>
#include "option.h"
#include "iwa.h"

#define	SAFE_RELEASE(obj)	if ((obj) != NULL) { (obj)->release(obj); (obj) = NULL; }

typedef struct {
	char *input;
	char *model;
	int evaluate;
	int quiet;
	int help;

	int num_params;
	char **params;

	FILE *fpi;
	FILE *fpo;
	FILE *fpe;
} tagger_option_t;

static void tagger_option_init(tagger_option_t* opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->fpi = stdin;
	opt->fpo = stdout;
	opt->fpe = stderr;
}

static void tagger_option_finish(tagger_option_t* opt)
{
	int i;

	free(opt->input);
	free(opt->model);
	for (i = 0;i < opt->num_params;++i) {
		free(opt->params[i]);
	}
	free(opt->params);
}

BEGIN_OPTION_MAP(parse_tagger_options, tagger_option_t)

	ON_OPTION_WITH_ARG(SHORTOPT('m') || LONGOPT("model"))
		free(opt->model);
		opt->model = strdup(arg);

	ON_OPTION(SHORTOPT('t') || LONGOPT("test"))
		opt->evaluate = 1;

	ON_OPTION(SHORTOPT('q') || LONGOPT("quiet"))
		opt->quiet = 1;

	ON_OPTION(SHORTOPT('h') || LONGOPT("help"))
		opt->help = 1;

	ON_OPTION_WITH_ARG(SHORTOPT('p') || LONGOPT("param"))
		opt->params = (char **)realloc(opt->params, sizeof(char*) * (opt->num_params + 1));
		opt->params[opt->num_params] = strdup(arg);
		++opt->num_params;

END_OPTION_MAP()

static void show_usage(FILE *fp, const char *argv0, const char *command)
{
	fprintf(fp, "USAGE: %s %s [OPTIONS] [DATA]\n", argv0, command);
	fprintf(fp, "Assign suitable labels to the instances in the data set given by a file (DATA).\n");
	fprintf(fp, "If the argument DATA is omitted or '-', this utility reads a data from STDIN.\n");
	fprintf(fp, "Evaluate the performance of the model on labeled instances (with -t option).\n");
	fprintf(fp, "\n");
	fprintf(fp, "OPTIONS:\n");
	fprintf(fp, "    -m, --model=MODEL   Read a model from a file (MODEL)\n");
	fprintf(fp, "    -t, --test          Report the performance of the model on the data\n");
	fprintf(fp, "    -q, --quiet         Suppress tagging results (useful for test mode)\n");
	fprintf(fp, "    -h, --help          Show the usage of this command and exit\n");
}



typedef struct {
	char **array;
	int num;
	int max;
} comments_t;

static void comments_init(comments_t* comments)
{
	memset(comments, 0, sizeof(*comments));
}

static void comments_finish(comments_t* comments)
{
	int i;

	for (i = 0;i < comments->num;++i) {
		free(comments->array[i]);
	}
	free(comments->array);
	comments_init(comments);
}

static int comments_append(comments_t* comments, const char *value)
{
	int i;

	if (comments->max <= comments->num) {
		comments->max = (comments->max + 1) * 2;
		comments->array = realloc(comments->array, sizeof(char*) * comments->max);
		if (comments->array == NULL) {
			return 1;
		}

		for (i = comments->num;i < comments->max;++i) {
			comments->array[i] = NULL;
		}
	}

	comments->array[comments->num++] = strdup(value);
	return 0;
}



static void
output_result(
	FILE *fpo,
	crf_output_t *output,
	crf_dictionary_t *labels,
	comments_t* comments)
{
	int i;

	for (i = 0;i < output->num_labels;++i) {
		char *label = NULL;
		labels->to_string(labels, output->labels[i], &label);
		fprintf(fpo, "%s", label);
		labels->free(labels, label);

		if (i < comments->num && comments->array[i] != NULL) {
			fprintf(fpo, "\t%s\n", comments->array[i]);
		} else {
			fprintf(fpo, "\n");
		}
	}
	fprintf(fpo, "\n");
}

static int tag(tagger_option_t* opt, crf_model_t* model)
{
	int N = 0, L = 0, ret = 0, lid = -1;
	clock_t clk0, clk1;
	crf_instance_t inst;
	crf_item_t item;
	crf_content_t cont;
	crf_output_t output;
	crf_evaluation_t eval;
	char *comment = NULL;
	comments_t comments;
	iwa_t* iwa = NULL;
	const iwa_token_t* token = NULL;
	crf_tagger_t *tagger = NULL;
	crf_dictionary_t *attrs = NULL, *labels = NULL;
	FILE *fp = NULL, *fpi = opt->fpi, *fpo = opt->fpo, *fpe = opt->fpe;

	/* Obtain the dictionary interface representing the labels in the model. */
	if (ret = model->get_labels(model, &labels)) {
		goto force_exit;
	}

	/* Obtain the dictionary interface representing the attributes in the model. */
	if (ret = model->get_attrs(model, &attrs)) {
		goto force_exit;
	}

	/* Obtain the tagger interface. */
	if (ret = model->get_tagger(model, &tagger)) {
		goto force_exit;
	}

	/* Initialize the objects for instance and evaluation. */
	L = labels->num(labels);
	crf_instance_init(&inst);
	crf_evaluation_init(&eval, L);

	/* Open the stream for the input data. */
	fp = (strcmp(opt->input, "-") == 0) ? fpi : fopen(opt->input, "r");
	if (fp == NULL) {
		fprintf(fpe, "ERROR: failed to open the stream for the input data,\n");
		fprintf(fpe, "  %s\n", opt->input);
		ret = 1;
		goto force_exit;
	}

	/* Open a IWA reader. */
	iwa = iwa_reader(fp);
	if (iwa == NULL) {
		fprintf(fpe, "ERROR: Failed to initialize the parser for the input data.\n");
		ret = 1;
		goto force_exit;
	}

	/* Read the input data and assign labels. */
	comments_init(&comments);
	clk0 = clock();
	while (token = iwa_read(iwa), token != NULL) {
		switch (token->type) {
		case IWA_BOI:
			printf("BOI\n");
			/* Initialize an item. */
			lid = -1;
			crf_item_init(&item);
			free(comment);
			comment = NULL;
			break;
		case IWA_EOI:
			printf("EOI\n");
			/* Skip an item with no content. */
			if (!crf_item_empty(&item)) {
				/* Append the item to the instance. */
				crf_instance_append(&inst, &item, lid);
				comments_append(&comments, comment);
			}
			crf_item_finish(&item);
			break;
		case IWA_ITEM:
			if (lid == -1) {
				/* The first field in a line presents a label. */
				lid = labels->to_id(labels, token->attr);
				if (lid < 0) lid = L;	/* #L stands for a unknown label. */
			} else {
				/* Fields after the first field present attributes. */
				int aid = attrs->to_id(attrs, token->attr);
				/* Ignore attributes 'unknown' to the model. */
				if (0 <= aid) {
					/* Associate the attribute with the current item. */
					crf_content_set(&cont, aid, 1.0);
					crf_item_append_content(&item, &cont);
				}
			}
			break;
		case IWA_NONE:
		case IWA_EOF:
			printf("NONE\n");
			if (!crf_instance_empty(&inst)) {
				/* Initialize the object to receive the tagging result. */
				crf_output_init(&output);

				/* Tag the instance. */
				if (ret = tagger->tag(tagger, &inst, &output)) {
					goto force_exit;
				}
				++N;

				/* Accumulate the tagging performance. */
				if (opt->evaluate) {
					crf_evaluation_accmulate(&eval, &inst, &output);
				}

				if (!opt->quiet) {
					output_result(fpo, &output, labels, &comments);
				}

				crf_output_finish(&output);
				crf_instance_finish(&inst);

				comments_finish(&comments);
				comments_init(&comments);
			}
			break;
		case IWA_COMMENT:
			comment = strdup(token->comment);
			break;
		}
	}
	clk1 = clock();

	/* Compute the performance if specified. */
	if (opt->evaluate) {
		double sec = (clk1 - clk0) / (double)CLOCKS_PER_SEC;
		crf_evaluation_compute(&eval);
		crf_evaluation_output(&eval, labels, fpo);
		fprintf(fpo, "Elapsed time: %f [sec] (%.1f [instance/sec])\n", sec, N / sec);
	}

force_exit:
	/* Close the IWA parser. */
	iwa_delete(iwa);
	iwa = NULL;

	/* Close the input stream if necessary. */
	if (fp != NULL && fp != fpi) {
		fclose(fp);
		fp = NULL;
	}

	free(comment);
	crf_instance_finish(&inst);
	crf_evaluation_finish(&eval);

	SAFE_RELEASE(tagger);
	SAFE_RELEASE(attrs);
	SAFE_RELEASE(labels);

	return ret;
}

int main_tag(int argc, char *argv[], const char *argv0)
{
	int ret = 0, arg_used = 0;
	tagger_option_t opt;
	const char *command = argv[0];
	FILE *fp = NULL, *fpi = stdin, *fpo = stdout, *fpe = stderr;
	crf_model_t *model = NULL;

	/* Parse the command-line option. */
	tagger_option_init(&opt);
	arg_used = option_parse(++argv, --argc, parse_tagger_options, &opt);
	if (arg_used < 0) {
		ret = 1;
		goto force_exit;
	}

	/* Show the help message for this command if specified. */
	if (opt.help) {
		show_usage(fpo, argv0, command);
		goto force_exit;
	}

	/* Set an input file. */
	if (arg_used < argc) {
		opt.input = strdup(argv[arg_used]);
	} else {
		opt.input = strdup("-");	/* STDIN. */
	}

	/* Read the model. */
	if (opt.model != NULL) {
		/* Create a model instance corresponding to the model file. */
		if (ret = crf_create_instance_from_file(opt.model, &model)) {
			goto force_exit;
		}

		/* Tag the input data. */
		if (ret = tag(&opt, model)) {
			goto force_exit;
		}
	}

force_exit:
	SAFE_RELEASE(model);
	tagger_option_finish(&opt);
	return ret;
}