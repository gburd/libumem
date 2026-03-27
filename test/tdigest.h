/*
 * t-digest: Efficient percentile estimation
 */

#ifndef TDIGEST_H
#define TDIGEST_H

#include <stddef.h>

#ifndef MM_PI
#define MM_PI 3.14159265358979323846
#endif

typedef struct td_histogram {
    double compression;
    int cap;
    int merged_nodes;
    long long merged_weight;
    int unmerged_nodes;
    long long unmerged_weight;
    double min;
    double max;
    long long total_compressions;
    double *nodes_mean;
    long long *nodes_weight;
} td_histogram_t;

/* Initialize a new t-digest with given compression factor */
int td_init(double compression, td_histogram_t **result);
td_histogram_t *td_new(double compression);

/* Free a t-digest */
void td_free(td_histogram_t *histogram);

/* Reset a t-digest to empty state */
void td_reset(td_histogram_t *h);

/* Add a value with given weight */
int td_add(td_histogram_t *h, double mean, long long weight);

/* Merge another t-digest into this one */
int td_merge(td_histogram_t *into, td_histogram_t *from);

/* Compress the digest (consolidate centroids) */
int td_compress(td_histogram_t *h);

/* Query functions */
double td_quantile(td_histogram_t *h, double q);
int td_quantiles(td_histogram_t *h, const double *quantiles, double *values, size_t length);
double td_cdf(td_histogram_t *h, double val);
double td_trimmed_mean(td_histogram_t *h, double leftmost_cut, double rightmost_cut);
double td_trimmed_mean_symmetric(td_histogram_t *h, double proportion_to_cut);

/* Statistics */
long long td_size(td_histogram_t *h);
double td_min(td_histogram_t *h);
double td_max(td_histogram_t *h);
int td_compression(td_histogram_t *h);
int td_centroid_count(td_histogram_t *h);

/* Centroid access */
const long long *td_centroids_weight(td_histogram_t *h);
const double *td_centroids_mean(td_histogram_t *h);
long long td_centroids_weight_at(td_histogram_t *h, int pos);
double td_centroids_mean_at(td_histogram_t *h, int pos);

#endif /* TDIGEST_H */
