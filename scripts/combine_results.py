#!/usr/bin/env python3
"""
Combine multiple results.json files from parallel benchmark runs into a single results.json

This script is designed for CI pipelines where different scenarios are run in parallel jobs.
Each job produces its own results.json file, and this script merges them into one combined file.
"""

import json
import sys
import argparse
from pathlib import Path
from typing import List, Dict


def load_results_file(file_path: Path) -> List[Dict]:
    """Load a results.json file and return the list of result dictionaries"""
    try:
        with open(file_path, 'r') as f:
            data = json.load(f)
            if not isinstance(data, list):
                print(f"Warning: {file_path} does not contain a list, skipping")
                return []
            return data
    except json.JSONDecodeError as e:
        print(f"Error: Failed to parse {file_path}: {e}")
        return []
    except Exception as e:
        print(f"Error: Failed to read {file_path}: {e}")
        return []


def combine_results(input_files: List[Path]) -> List[Dict]:
    """Combine multiple results.json files into a single list"""
    combined = []

    for file_path in input_files:
        if not file_path.exists():
            print(f"Warning: File not found: {file_path}")
            continue

        print(f"Loading: {file_path}")
        results = load_results_file(file_path)

        if results:
            combined.extend(results)
            print(f"  Added {len(results)} results")

    return combined


def sort_results(results: List[Dict]) -> List[Dict]:
    """Sort results by scenario work duration and method for consistent ordering"""
    # Define method order
    method_order = {'baseline': 0, 'lttng': 1, 'ebpf': 2}

    def sort_key(result):
        work_us = result.get('simulated_work_us', 0)
        method = result.get('method', '')
        method_idx = method_order.get(method, 999)
        return (work_us, method_idx)

    return sorted(results, key=sort_key)


def validate_results(results: List[Dict]) -> bool:
    """Validate that combined results have the expected structure"""
    if not results:
        print("Error: No results to validate")
        return False

    # Check that we have results for each scenario
    scenarios = set(r.get('simulated_work_us') for r in results)
    methods = set(r.get('method') for r in results)

    print(f"\nValidation:")
    print(f"  Scenarios found: {sorted(scenarios)}")
    print(f"  Methods found: {sorted(methods)}")
    print(f"  Total results: {len(results)}")

    # Expected: 6 scenarios × 3 methods = 18 results
    expected_scenarios = {0, 5, 50, 100, 500, 1000}
    expected_methods = {'baseline', 'lttng', 'ebpf'}

    if scenarios != expected_scenarios:
        print(f"Warning: Expected scenarios {expected_scenarios}, got {scenarios}")

    if methods != expected_methods:
        print(f"Warning: Expected methods {expected_methods}, got {methods}")

    return True


def main():
    parser = argparse.ArgumentParser(
        description='Combine multiple results.json files from parallel benchmark runs',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Combine results from multiple files
  %(prog)s -i scenario_0/results.json scenario_1/results.json -o combined_results.json

  # Combine all results.json files in a directory
  %(prog)s -d benchmark_artifacts -o combined_results.json

  # Combine and validate
  %(prog)s -i results_*.json -o combined.json --validate
        '''
    )

    parser.add_argument(
        '-i', '--input-files',
        type=str,
        nargs='+',
        help='Input results.json files to combine'
    )

    parser.add_argument(
        '-d', '--input-dir',
        type=str,
        help='Directory containing results.json files (will search recursively)'
    )

    parser.add_argument(
        '-o', '--output',
        type=str,
        required=True,
        help='Output file path for combined results.json'
    )

    parser.add_argument(
        '--validate',
        action='store_true',
        help='Validate combined results'
    )

    parser.add_argument(
        '--sort',
        action='store_true',
        default=True,
        help='Sort results by scenario and method (default: enabled)'
    )

    args = parser.parse_args()

    # Collect input files
    input_files = []

    if args.input_files:
        input_files.extend([Path(f) for f in args.input_files])

    if args.input_dir:
        input_dir = Path(args.input_dir)
        if not input_dir.exists():
            print(f"Error: Input directory not found: {input_dir}")
            sys.exit(1)

        # Find all results.json files recursively
        found_files = list(input_dir.rglob('results.json'))
        input_files.extend(found_files)
        print(f"Found {len(found_files)} results.json files in {input_dir}")

    if not input_files:
        print("Error: No input files specified. Use -i or -d to specify input files.")
        sys.exit(1)

    # Remove duplicates and sort
    input_files = sorted(set(input_files))

    print(f"\n{'='*70}")
    print("COMBINING BENCHMARK RESULTS")
    print('='*70)
    print(f"Input files: {len(input_files)}")

    # Combine results
    combined_results = combine_results(input_files)

    if not combined_results:
        print("\nError: No results were loaded")
        sys.exit(1)

    # Sort results
    if args.sort:
        print("\nSorting results...")
        combined_results = sort_results(combined_results)

    # Validate if requested
    if args.validate:
        if not validate_results(combined_results):
            print("\nWarning: Validation found issues")

    # Write combined results
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"\nWriting combined results to: {output_path}")
    with open(output_path, 'w') as f:
        json.dump(combined_results, f, indent=2)

    print(f"\n{'='*70}")
    print("✅ RESULTS COMBINED SUCCESSFULLY!")
    print('='*70)
    print(f"Combined {len(combined_results)} results from {len(input_files)} files")
    print(f"Output: {output_path.absolute()}")
    print()


if __name__ == '__main__':
    main()
