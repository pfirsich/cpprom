#!/bin/bash
prometheus --config.file=prometheus.yml --storage.tsdb.path="./prometheus_data/"
