#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "filesystem.h"

using int64  = int64_t;
using uint64 = uint64_t;
using uint   = uint32_t;
using ushort = uint16_t;
using byte   = int8_t;
using ubyte  = uint8_t;

constexpr int PVMX_FOURCC = 'XMVP';
static const ubyte PVMX_VERSION = 1;

constexpr auto npos = std::string::npos;

struct TexPackEntry
{
	uint32_t    global_index = 0;
	std::string name;
	uint32_t    width  = 0;
	uint32_t    height = 0;
};

struct DictionaryEntry : TexPackEntry
{
	uint64 offset = 0;
	uint64 size = 0;
};

namespace dictionary_field
{
	enum _ : ubyte
	{
		none,
		/**
		 * \brief 32-bit integer global index
		 */
		global_index,
		/**
		 * \brief Null-terminated file name
		 */
		name,
		/**
		 * \brief Two 32-bit integers defining width and height
		 */
		dimensions,
	};

	static_assert(sizeof(none) == sizeof(ubyte), "dictionary_field size mismatch");
}

static void usage()
{
	using namespace std;

	cout << "Usage:" << endl
		<< "\t-c, --create     Create an archive using the given texture pack index." << endl
		<< "\t-e, --extract    Extract an archive." << endl
		<< "\t-o, --output     Output file for creation or output directory for extraction." << endl;
}

#pragma region convenience

template <typename T>
auto& write_t(std::ofstream& file, const T& data)
{
	return file.write(reinterpret_cast<const char*>(&data), sizeof(T));
}

template <typename T>
auto& read_t(std::ifstream& file, T& data)
{
	return file.read(reinterpret_cast<char*>(&data), sizeof(T));
}

void read_cstr(std::ifstream& file, std::string& out)
{
	std::stringstream buffer;

	while (true)
	{
		char c;
		read_t(file, c);

		if (c == '\0')
		{
			break;
		}

		buffer.put(c);
	}

	out = buffer.str();
}

#pragma endregion

static void create_archive(const std::string& input_path, const std::string& output_path)
{
	using namespace std;

	string _input_path;

	if (filesystem::directory_exists(input_path))
	{
		_input_path = filesystem::combine_path(input_path, "index.txt");
	}
	else
	{
		_input_path = input_path;
	}

	if (!filesystem::file_exists(_input_path))
	{
		cout << "File not found: " << input_path << endl;
		return;
	}

	ifstream index_file(_input_path);

	if (!index_file.is_open())
	{
		cout << "Failed to open index file: " << _input_path << endl;
		return;
	}

	string _output_path;

	if (output_path.empty())
	{
		const auto path = filesystem::combine_path(filesystem::get_working_directory(), _input_path);
		const auto dir  = filesystem::get_directory(path);

		_output_path = filesystem::combine_path(filesystem::get_directory(dir), filesystem::get_base_name(dir) + ".pvmx");
	}
	else
	{
		_output_path = output_path;
	}

	ofstream out_file(_output_path, ios::out | ios::binary);

	if (!out_file.is_open())
	{
		cout << "Failed to create file: " << output_path << endl;
		return;
	}

	vector<string> files;
	vector<size_t> dict_offsets;

	// FourCC
	write_t(out_file, PVMX_FOURCC);
	// Archive version
	write_t(out_file, PVMX_VERSION);

	uint line_number = 0;
	string line;

	while (!index_file.eof())
	{
		try
		{
			++line_number;
			getline(index_file, line);

			if (line.length() == 0 || line[0] == '#')
			{
				continue;
			}

			size_t comma = line.find(',');

			if (comma < 1)
			{
				printf("Invalid texture index entry on line %u (missing comma?)\n", line_number);
				return;
			}

			uint width  = 0;
			uint height = 0;

			uint gbix = stoul(line.substr(0, comma));
			string name = line.substr(comma + 1);

			comma = name.find(',');

			// check for an additional texture dimensions field
			if (comma != npos && comma > 0)
			{
				string tmp = name;
				name = tmp.substr(0, comma);

				string dimensions = tmp.substr(++comma);
				size_t separator  = dimensions.find('x');

				// If no 'x' separator is found, try capital
				if (!separator || separator == npos)
				{
					separator = dimensions.find('X');
				}

				if (!separator || separator == npos)
				{
					printf("Invalid format for texture dimensions on line %u: %s\n",
					       line_number, dimensions.c_str());
					return;
				}

				width  = stoul(dimensions.substr(0, separator));
				height = stoul(dimensions.substr(++separator));
			}

			files.push_back(name);

			write_t(out_file, dictionary_field::global_index);
			write_t(out_file, gbix);

			write_t(out_file, dictionary_field::name);
			out_file << name << '\0';

			if (width || height)
			{
				write_t(out_file, dictionary_field::dimensions);
				write_t(out_file, width);
				write_t(out_file, height);
			}

			write_t(out_file, dictionary_field::none);

			dict_offsets.push_back(out_file.tellp());

			// Offset (to be populated later)
			write_t(out_file, static_cast<uint64>(0));
			// Size (to be populated later)
			write_t(out_file, static_cast<uint64>(0));
		}
		catch (exception& exception)
		{
			printf("An exception occurred while parsing texture index on line %u: %s\n", line_number, exception.what());
			return;
		}
	}

	// Dictionary element starting with 0 marks the end of dictionary.
	write_t(out_file, dictionary_field::none);

	// Tracks file offsets (first) and sizes (second)
	unordered_map<string, pair<size_t, size_t>> file_meta;

	const string dir = filesystem::get_directory(_input_path);

	char buffer[4096] = {};
	static_assert(sizeof(buffer) == 4096, "nope");

	// Write file data to the data section of the archive
	for (const auto& file : files)
	{
		const auto it = file_meta.find(file);

		if (it != file_meta.end())
		{
			continue;
		}

		const string path = filesystem::combine_path(dir, file);
		ifstream in_file(path, ios::in | ios::binary | ios::ate);

		if (!in_file.is_open())
		{
			cout << "Unable to open file: " << path << endl;
			return;
		}

		auto size = static_cast<size_t>(in_file.tellg());

		file_meta[file] = make_pair(static_cast<size_t>(out_file.tellp()), size);
		in_file.seekg(0);

		while (!in_file.eof())
		{
			const auto start = static_cast<size_t>(in_file.tellg());

			if (start == size)
			{
				break;
			}

			in_file.read(buffer, min(sizeof(buffer), size - start));

			const auto end   = static_cast<size_t>(in_file.tellg());
			const auto delta = end - start;

			out_file.write(buffer, delta);
		}
	}

	// Write file offsets and sizes to the dictionary
	for (size_t i = 0; i < files.size(); i++)
	{
		out_file.seekp(dict_offsets[i]);

		auto& pair = file_meta[files[i]];
		write_t(out_file, pair.first);  // offset
		write_t(out_file, pair.second); // size
	}
}

static void extract_archive(const std::string& input_path, const std::string& output_path)
{
	using namespace std;

	ifstream in_file(input_path, ios::binary | ios::in);

	if (!in_file.is_open())
	{
		cout << "Unable to open input file: " << input_path << endl;
		return;
	}

	string _output_path;

	if (output_path.empty())
	{
		const string path = filesystem::combine_path(filesystem::get_working_directory(), input_path);
		const string dir  = filesystem::get_directory(path);

		string name = filesystem::get_base_name(path);
		filesystem::strip_extension(name);

		_output_path = filesystem::combine_path(dir, name);
	}
	else
	{
		_output_path = output_path;
	}

	if (!filesystem::directory_exists(_output_path))
	{
		if (!CreateDirectoryA(_output_path.c_str(), nullptr))
		{
			cout << "Unable to create output directory: " << _output_path << endl;
			return;
		}
	}

	const string index_path = filesystem::combine_path(_output_path, "index.txt");
	ofstream index_file(index_path);

	if (!index_file.is_open())
	{
		cout << "Unable to create index file: " << index_path << endl;
		return;
	}

	int fourcc;
	ubyte version;

	read_t(in_file, fourcc);

	if (fourcc != PVMX_FOURCC)
	{
		cout << "File is not a PVMX archive." << endl;
		return;
	}

	read_t(in_file, version);

	if (version != PVMX_VERSION)
	{
		cout << "Incorrect PVMX archive version." << endl;
		return;
	}

	vector<DictionaryEntry> entries;
	ubyte type = 0;

	for (read_t(in_file, type); type != dictionary_field::none; read_t(in_file, type))
	{
		bool has_gbix = false;
		DictionaryEntry entry = {};

		while (type != dictionary_field::none)
		{
			switch (type)
			{
				case dictionary_field::global_index:
					read_t(in_file, entry.global_index);
					has_gbix = true;
					break;

				case dictionary_field::name:
					read_cstr(in_file, entry.name);
					break;

				case dictionary_field::dimensions:
					read_t(in_file, entry.width);
					read_t(in_file, entry.height);
					break;

				default:
					break;
			}

			read_t(in_file, type);
		}

		read_t(in_file, entry.offset);
		read_t(in_file, entry.size);

		entries.push_back(entry);

		int n = 0;

		if (has_gbix)
		{
			index_file << entry.global_index;
			++n;
		}

		if (!entry.name.empty())
		{
			if (n++)
			{
				index_file << ',';
			}

			index_file << entry.name;
		}

		if (entry.width || entry.height)
		{
			if (n)
			{
				index_file << ',';
			}

			index_file << entry.width << 'x' << entry.height;
		}

		index_file << endl;
	}

	char buffer[4096] = {};
	static_assert(sizeof(buffer) == 4096, "nope");

	for (auto& i : entries)
	{
		cout << "Extracting: " << i.name << endl;

		const string path = filesystem::combine_path(_output_path, i.name);
		ofstream out_file(path, ios::binary | ios::out);

		if (!out_file.is_open())
		{
			cout << "Unable to open output file: " << path << endl;
			return;
		}

		in_file.seekg(i.offset);
		auto position = static_cast<uint64>(in_file.tellg());
		const auto end = i.offset + i.size;

		while (position < end)
		{
			const auto start = position;

			in_file.read(buffer, min(static_cast<uint64>(sizeof(buffer)), end - start));
			position = static_cast<uint64>(in_file.tellg());
			
			const auto delta = position - start;

			out_file.write(buffer, delta);
		}
	}
}

int main(int argc, char** argv)
{
	using namespace std;

	if (argc < 2)
	{
		usage();
		return 0;
	}

	bool create  = false;
	bool extract = false;

	string input_path;
	string output_path;

	for (int i = 1; i < argc; i++)
	{
		const auto arg = argv[i];

		if (!_strcmpi("--help", arg) || !_strcmpi("-h", arg) || !_strcmpi("-?", arg))
		{
			usage();
			return 0;
		}

		if (!_strcmpi("--create", arg) || !_strcmpi("-c", arg))
		{
			if (++i >= argc)
			{
				cout << "--create: no input path specified." << endl;
				return -1;
			}

			create = true;
			input_path = argv[i];
			continue;
		}

		if (!_strcmpi("--extract", arg) || !_strcmpi("-e", arg))
		{
			if (++i >= argc)
			{
				cout << "--extract: no input path specified." << endl;
				return -1;
			}

			extract = true;
			input_path = argv[i];
			continue;
		}

		if (!_strcmpi("--output", arg) || !_strcmpi("-o", arg))
		{
			if (++i >= argc)
			{
				cout << "--output: no output path specified." << endl;
				return -1;
			}

			output_path = argv[i];
		}
	}

	if (input_path.empty())
	{
		cout << "Input path cannot be empty." << endl;
		usage();
		return -1;
	}

	if (create)
	{
		create_archive(input_path, output_path);
	}
	else if (extract)
	{
		extract_archive(input_path, output_path);
	}

	return 0;
}
