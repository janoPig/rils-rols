#include <iostream>
#include <tuple>
#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <random>
#include <chrono>
#include "node.h"
#include "utils.h"

using namespace std;
using namespace std::chrono;
namespace fs = std::filesystem;

enum fitness_type
{
	BIC = 1,
	SRM = 2,
	PENALTY = 3,
	JACCARD = 4
};

template<int M, template<typename> class F = std::less>
struct TupleCompare
{
	template<typename T>
	bool operator()(T const& t1, T const& t2)
	{
		return F<typename tuple_element<M, T>::type>()(std::get<M>(t1), std::get<M>(t2));
	}
};

class rils_rols {

private:
	// control parameters
	int max_fit_calls, max_seconds, random_state;
	double complexity_penalty, initial_sample_size;
	bool random_perturbations_order, verbose;
	fitness_type fitness_type;

	// internal stuff
	int ls_it, main_it, last_improved_it, time_start, time_elapsed, fit_calls;
	unordered_map<string, tuple<double, double, int>> cache_fitness;
	int cache_hits;
	node final_solution;
	tuple<double, double, int> final_fitness;
	int best_time;
	int total_time;

	void reset() {
		ls_it = 0;
		main_it = 0;
		last_improved_it = 0;
		time_start = 0;
		time_elapsed = 0;
		fit_calls = 0;
		cache_hits = 0;
		cache_fitness.clear();
		srand(random_state);
	}

	vector<node> allowed_nodes;

	void setup_nodes(int var_cnt) {
		allowed_nodes.push_back(node::node_plus());
		allowed_nodes.push_back(node::node_minus());
		allowed_nodes.push_back(node::node_multiply());
		allowed_nodes.push_back(node::node_divide());
		allowed_nodes.push_back(node::node_sin());
		allowed_nodes.push_back(node::node_cos());
		allowed_nodes.push_back(node::node_ln());
		allowed_nodes.push_back(node::node_exp());
		allowed_nodes.push_back(node::node_sqrt());
		double constants[] = { -1, 0, 0.5, 2, M_PI, 10 };
		for (auto c : constants)
			allowed_nodes.push_back(node::node_constant(c));
		for (int i = 0; i < var_cnt; i++)
			allowed_nodes.push_back(node::node_variable(i));
	}

	vector<node> perturb_candidates(node* old_node) {
		vector<node> candidates;
		if (old_node->arity >= 1) {
			// change node to its left subtree
			node* left_c = node::node_copy(old_node->left);
			candidates.push_back(*left_c);
		}

		if (old_node->arity >= 2) {
			// change node to its right subtree
			node* right_c = node::node_copy(old_node->right);
			candidates.push_back(*right_c);
		}

		// change anything to variable
		for (auto& n : allowed_nodes) {
			if (n.type != node_type::VAR)
				continue;
			if (old_node->type == node_type::VAR && old_node->var_index == n.var_index)
				continue; // avoid changing to same variable
			node* n_c = node::node_copy(&n);
			candidates.push_back(*n_c);
		}

		// change anything to constants 0 or 1
		candidates.push_back(node::node_constant(0));
		candidates.push_back(node::node_constant(1));

		// change anything to unary operation applied to it
		for (auto& n_un : allowed_nodes) {
			if (n_un.arity != 1)
				continue;
			node* n_un_c = node::node_copy(&n_un);
			node* old_node_c = node::node_copy(old_node);
			n_un_c->left = old_node_c;
			candidates.push_back(*n_un_c);
		}

		if (old_node->arity == 1) {
			// change unary operation to another unary operation
			for (auto& n_un : allowed_nodes) {
				if (n_un.arity != 1 || n_un.type == old_node->type)
					continue;
				node* n_un_c = node::node_copy(&n_un);
				node* old_left_c = node::node_copy(old_node->left);
				n_un_c->left = old_left_c;
				candidates.push_back(*n_un_c);
			}
		}
		if (old_node->arity == 2) {
			// change one binary operation to another
			for (auto& n_bin : allowed_nodes) {
				if (n_bin.arity != 2 || n_bin.type == old_node->type)
					continue;
				node* n_bin_c = node::node_copy(&n_bin);
				node* old_left_c = node::node_copy(old_node->left);
				node* old_right_c = node::node_copy(old_node->right);
				n_bin_c->left = old_left_c;
				n_bin_c->right = old_right_c;
				candidates.push_back(*n_bin_c);
				if (!n_bin.symmetric) {
					node* n_bin_c = node::node_copy(&n_bin);
					node* old_left_c = node::node_copy(old_node->left);
					node* old_right_c = node::node_copy(old_node->right);
					n_bin_c->right = old_left_c;
					n_bin_c->left = old_right_c;
					candidates.push_back(*n_bin_c);
				}
			}
		}
		// change anything to binary operation with some variable or constant  -- increases the model size
		for (auto& n_bin : allowed_nodes) {
			if (n_bin.arity != 2)
				continue;
			for (auto& n_var : allowed_nodes) {
				if (n_var.type != node_type::VAR && n_var.type!=node_type::CONST)
					continue;
				node* n_bin_c = node::node_copy(&n_bin);
				node* old_node_c = node::node_copy(old_node);
				node* n_var_c = node::node_copy(&n_var);
				n_bin_c->left = old_node_c;
				n_bin_c->right = n_var_c;
				candidates.push_back(*n_bin_c);
				if (!n_bin.symmetric) {
					n_bin_c = node::node_copy(&n_bin);
					old_node_c = node::node_copy(old_node);
					n_var_c = node::node_copy(&n_var);
					n_bin_c->right = old_node_c;
					n_bin_c->left = n_var_c;
					candidates.push_back(*n_bin_c);
				}
			}
		}
		// TODO: check for duplicates
		return candidates;
	}

	vector<node> all_perturbations(node passed_solution) {
		node* solution = node::node_copy(&passed_solution);
		vector<node> all_pert;
		vector<node*> all_subtrees;
		all_subtrees.push_back(solution);
		int i = 0;
		while (i < all_subtrees.size()) {
			if (all_subtrees[i]->size() == solution->size()) {
				// the whole tree is being changed
				vector<node> candidates = perturb_candidates(all_subtrees[i]);
				for (auto& cand : candidates)
					all_pert.push_back(cand);
			}
			if (all_subtrees[i]->arity >= 1) {
				// the left subtree is being changed
				vector<node> candidates = perturb_candidates(all_subtrees[i]->left);
				node* old_left = node::node_copy(all_subtrees[i]->left);
				for (auto& cand : candidates) {
					node* cand_c = node::node_copy(&cand);
					all_subtrees[i]->left = cand_c;
					node* solution_copy = node::node_copy(solution);
					all_pert.push_back(*solution_copy);
				}
				all_subtrees[i]->left = old_left;
				all_subtrees.push_back(all_subtrees[i]->left);
			}
			if (all_subtrees[i]->arity >= 2) {
				// the right subtree is being changed
				vector<node> candidates = perturb_candidates(all_subtrees[i]->right);
				node* old_right = node::node_copy(all_subtrees[i]->right);
				for (auto& cand : candidates) {
					node* cand_c = node::node_copy(&cand);
					all_subtrees[i]->right = cand_c;
					node* solution_copy = node::node_copy(solution);
					all_pert.push_back(*solution_copy);
				}
				all_subtrees[i]->right = old_right;
				all_subtrees.push_back(all_subtrees[i]->right);
			}
			i++;
		}

		unordered_set<string> filtered_pert_strings;
		vector<node> filtered_pert;
		for (auto& node : all_pert) {
			string node_str = node.to_string();
			if (filtered_pert_strings.contains(node_str))
				continue;
			filtered_pert_strings.insert(node_str);
			filtered_pert.push_back(node);
		}
		cout << "Kept " << filtered_pert.size() << " out of " << all_pert.size() << endl;
		return filtered_pert;
	}

	node tune_constants(node solution, vector<vector<double>> X, vector<double> y) {
		node* best_solution = node::node_copy(&solution);
		tuple<double, double, int> best_fitness = fitness(*best_solution, X, y);
		double multipliers[] = { -1, 0.01, 0.1, 0.2, 0.5, 0.8, 0.9, 1, 1.1, 1.2, 2, M_PI, 5, 10, 20, 50, 100 };
		bool impr = true;
		while (impr) {
			impr = false;
			vector<node*> constants = best_solution->extract_constants_references();
			int best_i = -1;
			double best_value = 1;
			for (int i = 0; i < constants.size(); i++) {
				double old_value = constants[i]->const_value;
				for (auto m : multipliers) {
					// TODO: multiplier does not make sense if old_value is zero
					constants[i]->const_value = old_value * m;
					tuple<double, double, int> new_fitness = fitness(*best_solution, X, y);
					if (compare_fitness(new_fitness, best_fitness) < 0) {
						best_fitness = new_fitness;
						best_i = i;
						best_value = constants[i]->const_value;
					}
				}
				constants[i]->const_value = old_value;
			}
			if (best_i != -1) {
				impr = true;
				constants[best_i]->const_value = best_value;
				tuple<double, double, int> test_fitness = fitness(*best_solution, X, y);
				assert(compare_fitness(test_fitness, best_fitness) == 0);
				//cout << "Improved to " << best_solution->to_string() << endl;
			}
		}
		return *best_solution;
	}

	tuple<double, double, int> fitness(node solution, vector<vector<double>> X, vector<double> y) {
		fit_calls++;
		string solution_string = solution.to_string();
		auto it = cache_fitness.find(solution_string);
		if (it != cache_fitness.end()) {
			cache_hits++;
			return it->second;
		}
		vector<double> yp = solution.evaluate_all(X);
		double r2 = utils::R2(y, yp);
		double rmse = utils::RMSE(y, yp);
		int size = solution.size();
		tuple<double, double, int> fit = tuple<double, double, int>{ 1 - r2, rmse, size };
		cache_fitness.insert({ solution_string, fit });
		return fit;
	}

	double penalty_fitness(tuple<double, double, int> fit) {
		return (1 + get<0>(fit)) * (1 + get<1>(fit)) * (1 + get<2>(fit) * this->complexity_penalty);
	}

	int compare_fitness(tuple<double, double, int> fit1, tuple<double, double, int> fit2) {
		double fit1_tot, fit2_tot;
		switch (this->fitness_type)
		{
		case fitness_type::PENALTY:
			fit1_tot = penalty_fitness(fit1);
			fit2_tot = penalty_fitness(fit2);
			break;
		default:
			throw exception("Not implemented fitness type.");
		}
		if (fit1_tot < fit2_tot)
			return -1;
		if (fit1_tot > fit2_tot)
			return 1;
		return 0;
	}

public:
	rils_rols(int max_fit_calls, int max_seconds, enum fitness_type fitness_type, double complexity_penalty, double initial_sample_size, bool random_perturbations_order, bool verbose, int random_state) {
		this->max_fit_calls = max_fit_calls;
		this->max_seconds = max_seconds;
		this->fitness_type = fitness_type;
		this->complexity_penalty = complexity_penalty;
		this->initial_sample_size = initial_sample_size;
		this->random_perturbations_order = random_perturbations_order;
		this->verbose = verbose;
		this->random_state = random_state;
	}

	void fit(vector<vector<double>> X_all, vector<double> y_all) {
		auto start = high_resolution_clock::now();
		reset();
		// take sample only assuming X_all and y_all are already shuffled
		vector<vector<double>> X;
		vector<double> y;
		for (int i = 0; i < initial_sample_size * X_all.size(); i++) {
			X.push_back(X_all[i]);
			y.push_back(y_all[i]);
		}
		setup_nodes(X[0].size());
		final_solution = node::node_constant(0);
		final_fitness = fitness(final_solution, X, y);
		// main loop
		bool improved = true;
		while (fit_calls < max_fit_calls) {
			main_it += 1;
			node start_solution = final_solution;
			if (!improved) {
				// if there was no change in previous iteration, then the search is stuck in local optima so we make two consecutive random perturbations on the final_solution (best overall)
				vector<node> all_perts = all_perturbations(final_solution);
				vector<node> all_2_perts = all_perturbations(all_perts[rand() % all_perts.size()]);
				start_solution = all_2_perts[rand() % all_2_perts.size()];
				cout << "Randomized to " << start_solution.to_string() << endl;
			}
			improved = false;
			//if(main_it%100==0)
			cout << main_it << ".\tcache hits " << cache_hits << "/" << fit_calls << "\t" << get<0>(final_fitness) << "\t" << final_solution.to_string() << endl;
			vector<node> all_perts = all_perturbations(start_solution);
			vector<tuple<double, node>> r2_by_perts;
			//taking the best 1-pert change
			for (int i = 0; i < all_perts.size(); i++) {
				node pert = all_perts[i];
				node pert_tuned = tune_constants(pert, X, y);
				tuple<double, double, int> pert_tuned_fitness = fitness(pert_tuned, X, y);
				r2_by_perts.push_back(tuple<double, node>{get<0>(pert_tuned_fitness), pert_tuned});
				if (compare_fitness(pert_tuned_fitness, final_fitness) < 0) {
					improved = true;
					final_fitness = pert_tuned_fitness;
					final_solution = pert_tuned;
					cout << "New best in phase 1:\t" << get<0>(final_fitness) << "\t" << final_solution.to_string() << endl;
					auto stop = high_resolution_clock::now();
					best_time = duration_cast<seconds>(stop - start).count();
				}
			}
			//if (improved)
			//	continue;

			sort(r2_by_perts.begin(), r2_by_perts.end(), TupleCompare<0>());
			// taking the first 2-pert overall change
			for (int i = 0; i < r2_by_perts.size(); i++) {
				if (fit_calls > max_fit_calls)
					break; 
				node pert = get<1>(r2_by_perts[i]);
				double pert_r2 = get<0>(r2_by_perts[i]);
				cout << i << "/" << r2_by_perts.size() << ".\t" << pert_r2 << "\t" << pert.to_string() << endl;
				vector<node> pert_perts = all_perturbations(pert);
				// actually best for a given 1-pert change
				for (auto& pert_pert : pert_perts) {
					node pert_pert_tuned = tune_constants(pert_pert, X, y);
					tuple<double, double, int> pert_pert_tuned_fitness = fitness(pert_pert_tuned, X, y);
					//cout << get<0>(fit) <<"\t" << pert_pert.to_string() << endl;
					if (compare_fitness(pert_pert_tuned_fitness, final_fitness) < 0) {
						improved = true;
						final_fitness = pert_pert_tuned_fitness;
						final_solution = pert_pert_tuned;
						cout << "New best in phase 2:\t" << get<0>(final_fitness) << "\t" << final_solution.to_string() << endl;
						auto stop = high_resolution_clock::now();
						best_time = duration_cast<seconds>(stop - start).count();
					}
				}
				if (improved)
					break;
			}
		}
		auto stop = high_resolution_clock::now();
		total_time = duration_cast<seconds>(stop - start).count();
	}

	vector<double> predict(vector<vector<double>> X) {
		return final_solution.evaluate_all(X);
	}

	string get_model_string() {
		return final_solution.to_string();
	}

	int get_best_time() {
		return best_time;
	}

	int get_total_time() {
		return total_time;
	}
};

static vector<vector<double>> random_data(int rows, int cols, int min_val, int max_val, int random_state) {
	assert(max_val > min_val);
	srand(random_state);
	vector<vector<double>> data;
	for (int i = 0; i < rows; i++) {
		vector<double> row(cols);
		for (int j = 0; j < cols; j++) {
			double rv01 = rand() * 1.0 / RAND_MAX;
			row[j] = rv01 * (max_val - min_val) + min_val;
		}
		data.push_back(row);
	}
	return data;
}

tuple<vector<vector<double>>, vector<double>> sample_dataset(int rows, int cols, int min_val, int max_val, int random_state) {
	assert(max_val > min_val);
	srand(random_state);
	vector<vector<double>> X;
	for (int i = 0; i < rows; i++) {
		vector<double> row(cols);
		for (int j = 0; j < cols; j++) {
			double rv01 = rand() * 1.0 / RAND_MAX;
			row[j] = rv01 * (max_val - min_val) + min_val;
		}
		X.push_back(row);
	}
	vector<double> y;
	for (auto x : X)
		y.push_back(x[0] * x[1] + x[1] / x[0] + sin(x[0] + x[1]));
	return tuple<vector<vector<double>>, vector<double>>{X, y};
}

int main()
{
	int random_state = 23654;
	int max_fit = 1000000;
	int max_time = 300;
	double complexity_penalty = 0.001;
	double sample_size = 0.01;
	double train_share = 0.75;
	//tuple<vector<vector<double>>, vector<double>> dataset = sample_dataset(100, 2, -3, 5, random_state);
	//vector<vector<double>> X = get<0>(dataset);
	//vector<double> y = get<1>(dataset);
	string dir_path = "../paper_resources/random_12345_data";
	for (const auto& entry :  fs::directory_iterator(dir_path)) {
		//if (entry.path().compare("../paper_resources/random_12345_data\\random_04_01_0010000_04.data") != 0)
		//	continue;
		vector<vector<double>> X_train, X_test;
		vector<double> y_train, y_test;
		std::cout << entry.path() << std::endl;
		ifstream infile(entry.path());
		string line;
		vector<string> lines;
		while (getline(infile, line))
			lines.push_back(line);
		// shuffling for later split between training and test set
		shuffle(lines.begin(), lines.end(), default_random_engine(random_state));
		for (int i = 0; i < lines.size(); i++) {
			string line = lines[i];
			stringstream ss(line);
			vector<string> tokens;
			string tmp;
			while (getline(ss, tmp, '\t'))
				tokens.push_back(tmp);
			vector<double> x;
			for (int i = 0; i < tokens.size() - 1; i++) {
				string str(tokens[i]);
				x.push_back(stod(str));
			}
			string str(tokens[tokens.size() - 1]);
			if (i < train_share * lines.size()) {
				X_train.push_back(x);
				y_train.push_back(stod(str));
			}
			else {
				X_test.push_back(x);
				y_test.push_back(stod(str));
			}
		}
		rils_rols rr(max_fit, max_time, fitness_type::PENALTY, complexity_penalty, sample_size, false, true, random_state);
		rr.fit(X_train, y_train);
		vector<double> yp_test = rr.predict(X_test);
		double r2 = utils::R2(y_test, yp_test);
		double rmse = utils::RMSE(y_test, yp_test);
		ofstream out_file;
		stringstream ss;
		ss <<  entry << "\tR2=" << r2 << "\tRMSE=" << rmse << "\ttotal_time="<<rr.get_total_time() << "\tbest_time="<<rr.get_best_time() << "\tmodel = " << rr.get_model_string() << endl;
		cout << ss.str();
		out_file.open("out.txt", ios_base::app);
		out_file << ss.str();
		out_file.close();
	}
}
