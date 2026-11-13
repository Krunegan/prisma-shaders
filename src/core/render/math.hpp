// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	Just enough linear algebra for camera matrices. Column-major, like GL:
	element (row r, column c) is m[c * 4 + r].
*/
#include <cmath>

namespace prisma {

struct Vec3 {
	float x = 0, y = 0, z = 0;

	Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	Vec3 operator-() const { return {-x, -y, -z}; }
	float length() const { return std::sqrt(x * x + y * y + z * z); }
	Vec3 normalized() const
	{
		float l = length();
		return l > 0 ? *this * (1.f / l) : *this;
	}
};

struct Mat4 {
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

	static Mat4 from(const float *p)
	{
		Mat4 r;
		for (int i = 0; i < 16; i++)
			r.m[i] = p[i];
		return r;
	}

	static Mat4 translation(const Vec3 &t)
	{
		Mat4 r;
		r.m[12] = t.x;
		r.m[13] = t.y;
		r.m[14] = t.z;
		return r;
	}

	static Mat4 scale(float s)
	{
		Mat4 r;
		r.m[0] = r.m[5] = r.m[10] = s;
		return r;
	}

	float &at(int row, int col) { return m[col * 4 + row]; }
	float at(int row, int col) const { return m[col * 4 + row]; }

	Mat4 operator*(const Mat4 &o) const
	{
		Mat4 r;
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 4; col++) {
				float s = 0;
				for (int k = 0; k < 4; k++)
					s += at(row, k) * o.at(k, col);
				r.at(row, col) = s;
			}
		return r;
	}

	// General inverse (cofactors). Returns identity if singular.
	Mat4 inverse() const
	{
		const float *a = m;
		float inv[16];
		inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
			a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
		inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
			a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
		inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
			a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
		inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
			a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
		inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
			a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
		inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
			a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
		inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
			a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
		inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
			a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
		inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
			a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
		inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
			a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
		inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
			a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
		inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
			a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
		inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
			a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
		inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
			a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
		inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
			a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
		inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
			a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

		float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
		if (std::fabs(det) < 1e-20f)
			return Mat4{};
		Mat4 r;
		for (int i = 0; i < 16; i++)
			r.m[i] = inv[i] / det;
		return r;
	}
};

// Double precision, for positions far from the origin (Luanti worlds reach
// 31000 nodes) before they are made relative to the camera.
struct DMat4 {
	double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

	static DMat4 from(const float *p)
	{
		DMat4 r;
		for (int i = 0; i < 16; i++)
			r.m[i] = p[i];
		return r;
	}

	static DMat4 from(const Mat4 &a) { return from(a.m); }

	static DMat4 translation(double x, double y, double z)
	{
		DMat4 r;
		r.m[12] = x;
		r.m[13] = y;
		r.m[14] = z;
		return r;
	}

	static DMat4 scale(double s)
	{
		DMat4 r;
		r.m[0] = r.m[5] = r.m[10] = s;
		return r;
	}

	double &at(int row, int col) { return m[col * 4 + row]; }
	double at(int row, int col) const { return m[col * 4 + row]; }

	DMat4 operator*(const DMat4 &o) const
	{
		DMat4 r;
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 4; col++) {
				double s = 0;
				for (int k = 0; k < 4; k++)
					s += at(row, k) * o.at(k, col);
				r.at(row, col) = s;
			}
		return r;
	}

	// Point transform, ignoring the projective row.
	void apply(const double in[3], double out[3]) const
	{
		for (int r = 0; r < 3; r++)
			out[r] = at(r, 0) * in[0] + at(r, 1) * in[1] + at(r, 2) * in[2] + at(r, 3);
	}

	Mat4 to_float() const
	{
		Mat4 r;
		for (int i = 0; i < 16; i++)
			r.m[i] = (float)m[i];
		return r;
	}
};

} // namespace prisma
