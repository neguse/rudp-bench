package result

import (
	"encoding/csv"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// ReadCSVRow reads the first data row of a CSV file. Returns (nil, nil) if the
// file doesn't exist or contains only a header.
func ReadCSVRow(path string) (map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	defer f.Close()
	r := csv.NewReader(f)
	header, err := r.Read()
	if err != nil {
		return nil, nil // empty file
	}
	record, err := r.Read()
	if err != nil {
		return nil, nil // header-only
	}
	row := make(map[string]string, len(header))
	for i, col := range header {
		if i < len(record) {
			row[col] = record[i]
		}
	}
	return row, nil
}

// ReadCSVRows reads all rows from a CSV file, each as a map keyed by header.
func ReadCSVRows(path string) ([]map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	defer f.Close()
	r := csv.NewReader(f)
	header, err := r.Read()
	if err != nil {
		return nil, nil
	}
	var rows []map[string]string
	for {
		record, err := r.Read()
		if err != nil {
			break
		}
		row := make(map[string]string, len(header))
		for i, col := range header {
			if i < len(record) {
				row[col] = record[i]
			}
		}
		rows = append(rows, row)
	}
	return rows, nil
}

// readCSVHeader reads just the header row from a CSV file.
func readCSVHeader(path string) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	r := csv.NewReader(f)
	header, err := r.Read()
	if err != nil {
		return nil, err
	}
	return header, nil
}

// EnsureHeader creates the file with a CSV header if it doesn't exist or is empty.
func EnsureHeader(path string, fields []string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	info, err := os.Stat(path)
	if err == nil && info.Size() > 0 {
		return nil
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	if err := w.Write(fields); err != nil {
		return err
	}
	w.Flush()
	return w.Error()
}

// AppendRow appends a single row to a CSV file, ensuring the header exists.
func AppendRow(path string, fields []string, row map[string]string) error {
	if err := EnsureHeader(path, fields); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	record := make([]string, len(fields))
	for i, col := range fields {
		record[i] = row[col]
	}
	if err := w.Write(record); err != nil {
		return err
	}
	w.Flush()
	return w.Error()
}

// RemoveRows rewrites the CSV at path without the rows for which match
// returns true, preserving the original header. Returns the number of rows
// removed. A missing or empty file is a no-op.
// resume/再実行時に result.Append の追記で同一 run の行が重複しないよう、
// 事前に既存行を除去するために使う。
func RemoveRows(path string, match func(row map[string]string) bool) (int, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}
	r := csv.NewReader(f)
	header, err := r.Read()
	if err != nil {
		f.Close()
		return 0, nil // empty file
	}
	var kept []map[string]string
	removed := 0
	for {
		record, err := r.Read()
		if err != nil {
			break
		}
		row := make(map[string]string, len(header))
		for i, col := range header {
			if i < len(record) {
				row[col] = record[i]
			}
		}
		if match(row) {
			removed++
			continue
		}
		kept = append(kept, row)
	}
	f.Close()
	if removed == 0 {
		return 0, nil
	}
	if err := WriteCSV(path, header, kept); err != nil {
		return removed, err
	}
	return removed, nil
}

// CombineCSV globs the given patterns, combines all matching CSVs into one
// output file, and returns the total row count. The header is taken from the
// first file found.
func CombineCSV(patterns []string, out string) (int, error) {
	var paths []string
	for _, pat := range patterns {
		matches, err := filepath.Glob(pat)
		if err != nil {
			return 0, err
		}
		paths = append(paths, matches...)
	}
	sort.Strings(paths)

	var fieldnames []string
	var rows []map[string]string
	for _, p := range paths {
		f, err := os.Open(p)
		if err != nil {
			continue
		}
		r := csv.NewReader(f)
		header, err := r.Read()
		f.Close()
		if err != nil {
			continue
		}
		if fieldnames == nil {
			fieldnames = header
		}
		f2, err := os.Open(p)
		if err != nil {
			continue
		}
		r2 := csv.NewReader(f2)
		r2.Read() // skip header
		for {
			record, err := r2.Read()
			if err != nil {
				break
			}
			row := make(map[string]string, len(header))
			for i, col := range header {
				if i < len(record) {
					row[col] = record[i]
				}
			}
			rows = append(rows, row)
		}
		f2.Close()
	}
	if fieldnames == nil {
		return 0, nil
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return 0, err
	}
	f, err := os.Create(out)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	w.UseCRLF = false
	if err := w.Write(fieldnames); err != nil {
		return 0, err
	}
	for _, row := range rows {
		record := make([]string, len(fieldnames))
		for i, col := range fieldnames {
			record[i] = row[col]
		}
		if err := w.Write(record); err != nil {
			return 0, err
		}
	}
	w.Flush()
	return len(rows), w.Error()
}

// --- Utility conversion functions ---

// IntOrZero parses s as an integer, returning 0 on failure or empty string.
func IntOrZero(s string) int {
	if s == "" {
		return 0
	}
	// Handle float strings like "7.50" by parsing as float first.
	v, err := strconv.Atoi(s)
	if err == nil {
		return v
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0
	}
	return int(f)
}

// IntOrNone parses s as an integer. Returns (value, true) on success,
// (0, false) on failure or empty string.
func IntOrNone(s string) (int, bool) {
	if s == "" {
		return 0, false
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return 0, false
	}
	return v, true
}

// FloatOrNone parses s as a float64. Returns (value, true) on success,
// (0, false) on failure or empty string.
func FloatOrNone(s string) (float64, bool) {
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

// RatioStr formats num/den as a 4-decimal string. Returns "" if den <= 0.
func RatioStr(num, den int) string {
	if den <= 0 {
		return ""
	}
	return fmt.Sprintf("%.4f", float64(num)/float64(den))
}

// floatToStr formats a float64 with the given number of decimal places.
func floatToStr(v float64, places int) string {
	if places == 0 {
		return strconv.Itoa(int(math.Round(v)))
	}
	return fmt.Sprintf("%.*f", places, v)
}

// mapGet returns the value for key in m, or def if not present.
func mapGet(m map[string]string, key, def string) string {
	if m == nil {
		return def
	}
	v, ok := m[key]
	if !ok {
		return def
	}
	return v
}

// mapHas reports whether a key exists in the map.
func mapHas(m map[string]string, key string) bool {
	if m == nil {
		return false
	}
	_, ok := m[key]
	return ok
}

// WriteCSV writes rows to a CSV file with the given field names, using LF line endings.
func WriteCSV(path string, fields []string, rows []map[string]string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	w.UseCRLF = false
	if err := w.Write(fields); err != nil {
		return err
	}
	for _, row := range rows {
		record := make([]string, len(fields))
		for i, col := range fields {
			record[i] = row[col]
		}
		if err := w.Write(record); err != nil {
			return err
		}
	}
	w.Flush()
	return w.Error()
}

// containsStr checks if a string slice contains a given value.
func containsStr(ss []string, s string) bool {
	for _, v := range ss {
		if v == s {
			return true
		}
	}
	return false
}

// joinSemicolon joins strings with ";".
func joinSemicolon(ss []string) string {
	return strings.Join(ss, ";")
}
