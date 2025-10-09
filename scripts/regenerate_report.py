#!/usr/bin/env python3
"""
Regenerate HTML report from existing results.json file

This is useful when you want to update the report visualization or
fix rendering issues without re-running the entire benchmark.
"""

import json
import sys
import argparse
import importlib
from pathlib import Path
from datetime import datetime

# Import BenchmarkSuite and BenchmarkResult from benchmark module
# Force reload to ensure we get the latest code changes
if 'benchmark' in sys.modules:
    importlib.reload(sys.modules['benchmark'])

from benchmark import BenchmarkSuite, BenchmarkResult


def main():
    parser = argparse.ArgumentParser(
        description='Regenerate HTML report from existing results.json',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Regenerate report from results directory
  %(prog)s benchmark_results_20251009_174347

  # Regenerate report from specific results.json file
  %(prog)s benchmark_results_20251009_174347/results.json

  # Regenerate report in a different output directory
  %(prog)s results.json -o new_report_dir
        '''
    )

    parser.add_argument(
        'results_path',
        type=str,
        help='Path to results.json file or directory containing it'
    )

    parser.add_argument(
        '-o', '--output-dir',
        type=str,
        help='Output directory for the report (default: same as results.json location)'
    )

    args = parser.parse_args()

    # Determine results.json path
    results_path = Path(args.results_path)

    if results_path.is_dir():
        results_file = results_path / 'results.json'
    elif results_path.is_file() and results_path.name == 'results.json':
        results_file = results_path
    else:
        print(f"Error: Could not find results.json at {results_path}")
        sys.exit(1)

    if not results_file.exists():
        print(f"Error: Results file not found: {results_file}")
        sys.exit(1)

    # Determine output directory
    if args.output_dir:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
    else:
        output_dir = results_file.parent

    print(f"Loading results from: {results_file}")

    # Load results
    try:
        with open(results_file) as f:
            results_data = json.load(f)
    except Exception as e:
        print(f"Error loading results.json: {e}")
        sys.exit(1)

    # Create a suite instance (build_dir doesn't matter for report generation)
    suite = BenchmarkSuite(Path('.'), num_runs=1)

    # Convert to BenchmarkResult objects
    try:
        suite.results = [BenchmarkResult(**r) for r in results_data]
    except Exception as e:
        print(f"Error parsing results data: {e}")
        sys.exit(1)

    # Set output directory
    suite.output_dir = output_dir

    # Extract timestamp from directory name if possible
    if results_file.parent.name.startswith('benchmark_results_'):
        suite.timestamp = results_file.parent.name.replace('benchmark_results_', '')
    else:
        suite.timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    # Generate HTML report
    print(f"Generating HTML report in: {output_dir}")
    try:
        report_file = suite.generate_html_report()
        print(f"\n{'='*70}")
        print("✅ REPORT REGENERATED SUCCESSFULLY!")
        print('='*70)
        print(f"\nHTML Report: {report_file}")
        print(f"View in browser: file://{report_file.absolute()}\n")
    except Exception as e:
        print(f"\nError generating HTML report: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
