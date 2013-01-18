/* Copyright STIFTELSEN SINTEF 2012
 *
 * This file is part of the HPMC Library.
 *
 * HPMC is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * HPMC is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * HPMC.  If not, see <http://www.gnu.org/licenses/>.
 */
// Morphing algebraic shapes that emits particles.
//
// This example demonstrates using the surface generated by HPMC as input to a
// geometry shader that emits particles randomly over the surface. The particles
// are pulled by gravity, and uses the scalar field passed to HPMC to determine
// when particles hit the surface, and in this case, they bounce. To test if a
// particle hits the surface is done by evaluating the sign of the scalar field
// at the position of the particle at the beginning of the timestep and at the
// end. This approach is a bit too simple for these shapes, as they usually have
// a great deal of regions with multiple zeros, and this leads to the artefact
// of particles falling through the surface at some places.
//
// The following render loop is used:
// - Use HPMC to determine the iso-surface of the current scalar field
// - Render the iso surface, but tap vertex position and normals into a
//   transform feedback buffer.
// - Pass this buffer into a geometry shader that emits particles (points) at
//   some of the triangles, output stored in another transform feedback buffer.
// - Pass the particles from the previous frame into a geometry shader that
//   does a series of Euler-steps to integrate velocity and position, checking
//   for collisions in-between. The output of this pass is concatenated at the
//   end of the newly created particles using transform feedback.
// - Render the particles using a geometry shader that expands the point
//   positions into quadrilateral screen-aligned billboards.

#include <cmath>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <vector>
#include <sstream>
#include "../common/common.hpp"

using std::min;
using std::max;
using std::cerr;
using std::endl;
using std::vector;
using std::string;
using std::ofstream;

// adjust this variable to change the amount of particles
static int particle_flow = 4000;

int                             volume_size_x       = 64;
int                             volume_size_y       = 64;
int                             volume_size_z       = 64;
GLuint                          anim_result         = 0;
GLuint                          mc_tri_vao          = 0;
GLuint                          mc_tri_vbo          = 0;
GLsizei                         mc_tri_vbo_N        = 0;

GLuint particles_vao[2];
GLuint particles_vbo[2]; // two buffers which are used round-robin
GLuint particles_vbo_p;  // tells which of the two buffers that are current
GLuint particles_vbo_n;  // number of particles in buffer
GLuint particles_vbo_N;  // size of particle buffer

// -----------------------------------------------------------------------------
// a small vertex shader that calls the provided extraction function
GLuint onscreen_p;
GLint  onscreen_loc_P;
GLint  onscreen_loc_M;
GLint  onscreen_loc_NM;
GLint  onscreen_loc_shape;

// -----------------------------------------------------------------------------
GLuint emitter_p;
GLuint emitter_query;
GLint  emitter_loc_P;
GLint  emitter_loc_offset;
GLint  emitter_loc_threshold;

// --- particle animation shader program ---------------------------------------
GLuint anim_p;
GLuint anim_query;
GLint  anim_loc_dt;
GLint  anim_loc_iso;
GLint  anim_loc_P;
GLint  anim_loc_MV;
GLint  anim_loc_MV_inv;
GLint  anim_loc_NM;
GLint  anim_loc_shape;

// --- particle billboard render shader program --------------------------------
GLuint billboard_p;
GLint  billboard_loc_P;
GLint  billboard_loc_color;
struct HPMCConstants* hpmc_c;
struct HPMCIsoSurface* hpmc_h;
struct HPMCIsoSurfaceRenderer* hpmc_th;

namespace resources {
    extern std::string particles_fetch;
    extern std::string particles_shape_vs_150;
    extern std::string particles_shape_fs_150;
    extern std::string particles_emitter_vs_150;
    extern std::string particles_emitter_gs_150;
    extern std::string particles_anim_vs_150;
    extern std::string particles_anim_gs_150;
    extern std::string particles_billboard_vs_150;
    extern std::string particles_billboard_gs_150;
    extern std::string particles_billboard_fs_150;
}





void
printHelp( const std::string& appname )
{
    cerr << "HPMC demo application that visalizes morphing algebraic shapes that emits particles."<<endl<<endl;
    cerr << endl;
    cerr << "Requires OpenGL 3.2 or better." << endl;
    cerr << endl;
    cerr << "Usage: " << appname << " [options] xsize [ysize zsize] "<<endl<<endl;
    cerr << "where: xsize    The number of samples in the x-direction."<<endl;
    cerr << "       ysize    The number of samples in the y-direction."<<endl;
    cerr << "       zsize    The number of samples in the z-direction."<<endl;
    cerr << "Example usage:"<<endl;
    cerr << "    " << appname << " 64"<< endl;
    cerr << "    " << appname << " 64 128 64"<< endl;
    cerr << endl;
    printOptions();
}
void
init( int argc, char** argv )
{
    if( hpmc_target < HPMC_TARGET_GL32_GLSL150 ) {
        std::cerr << "This sample requires OpenGL 3.2 or better." << std::endl;
        exit( EXIT_FAILURE );
    }

    if( argc > 1 ) {
        volume_size_x = atoi( argv[1] );
    }
    if( argc > 3 ) {
        volume_size_y = atoi( argv[2] );
        volume_size_z = atoi( argv[3] );
    }
    else {
        volume_size_y = volume_size_x;
        volume_size_z = volume_size_x;
    }
    if( volume_size_x < 4 ) {
        cerr << "Volume size x < 4" << endl;
        exit( EXIT_FAILURE );
    }
    if( volume_size_y < 4 ) {
        cerr << "Volume size y < 4" << endl;
        exit( EXIT_FAILURE );
    }
    if( volume_size_z < 4 ) {
        cerr << "Volume size z < 4" << endl;
        exit( EXIT_FAILURE );
    }


    // --- create HistoPyramid -------------------------------------------------
    hpmc_c = HPMCcreateConstants( hpmc_target, hpmc_debug );
    hpmc_h = HPMCcreateIsoSurface( hpmc_c );

    HPMCsetLatticeSize( hpmc_h,
                        volume_size_x,
                        volume_size_y,
                        volume_size_z );

    HPMCsetGridSize( hpmc_h,
                     volume_size_x-1,
                     volume_size_y-1,
                     volume_size_z-1 );

    HPMCsetGridExtent( hpmc_h,
                       1.0f,
                       1.0f,
                       1.0f );

    HPMCsetFieldCustom( hpmc_h,
                        resources::particles_fetch.c_str(),
                        0,
                        GL_TRUE );

    // --- create traversal vertex shader --------------------------------------
    {
        hpmc_th = HPMCcreateIsoSurfaceRenderer( hpmc_h );

        char *traversal_code = HPMCisoSurfaceRendererShaderSource( hpmc_th );
        const char* vs_src[2] = {
            resources::particles_shape_vs_150.c_str(),
            traversal_code
        };
        const char* fs_src[1] = {
            resources::particles_shape_fs_150.c_str()
        };
        const GLchar* varyings[2] =  {
            "normal_cs",
            "position_cs"
        };

        GLuint vs = glCreateShader( GL_VERTEX_SHADER );
        glShaderSource( vs, 2, vs_src, NULL );
        compileShader( vs, "onscreen vertex shader" );

        GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
        glShaderSource( fs, 1, fs_src, NULL );
        compileShader( fs, "onscreen fragment shader" );

        onscreen_p = glCreateProgram();
        glAttachShader( onscreen_p, vs );
        glAttachShader( onscreen_p, fs );
        glTransformFeedbackVaryings( onscreen_p, 2, varyings, GL_INTERLEAVED_ATTRIBS );
        linkProgram( onscreen_p, "onscreen program" );
        onscreen_loc_P = glGetUniformLocation( onscreen_p, "P" );
        onscreen_loc_M = glGetUniformLocation( onscreen_p, "M" );
        onscreen_loc_NM = glGetUniformLocation( onscreen_p, "NM" );
        onscreen_loc_shape = glGetUniformLocation( onscreen_p, "shape" );

        // associate the linked program with the traversal handle
        HPMCsetIsoSurfaceRendererProgram( hpmc_th,
                                          onscreen_p,
                                          0, 1, 2 );

        glDeleteShader( vs );
        glDeleteShader( fs );
        free( traversal_code );
    }

    // --- set up particle emitter program -------------------------------------
    {
        const GLchar* vs_src[1] = {
            resources::particles_emitter_vs_150.c_str()
        };
        const GLchar* gs_src[1] = {
            resources::particles_emitter_gs_150.c_str()
        };
        const GLchar* varyings[3] = {
            "info",
            "vel",
            "pos"
        };

        GLint vs = glCreateShader( GL_VERTEX_SHADER );
        glShaderSource( vs, 1, vs_src, NULL );
        compileShader( vs, "emitter vertex shader" );

        GLint gs = glCreateShader( GL_GEOMETRY_SHADER_EXT );
        glShaderSource( gs, 1, gs_src, NULL );
        compileShader( gs, "emitter geometry shader" );

        emitter_p = glCreateProgram();
        glAttachShader( emitter_p, vs );
        glAttachShader( emitter_p, gs );
        glTransformFeedbackVaryings( emitter_p, 3, varyings, GL_INTERLEAVED_ATTRIBS );
        glBindAttribLocation( emitter_p, 0, "vbo_normal" );
        glBindAttribLocation( emitter_p, 1, "vbo_position" );
        linkProgram( emitter_p, "emitter program" );

        emitter_loc_P = glGetUniformLocation( emitter_p, "P" );
        emitter_loc_offset = glGetUniformLocation( emitter_p, "offset" );
        emitter_loc_threshold = glGetUniformLocation( emitter_p, "threshold" );

        glDeleteShader( vs );
        glDeleteShader( gs );
    }

    // --- set up particle animation program -----------------------------------
    {
        const GLchar* vs_src[1] = {
            resources::particles_anim_vs_150.c_str()
        };
        const GLchar* gs_src[2] =
        {
            resources::particles_anim_gs_150.c_str(),
            resources::particles_fetch.c_str()
        };
        const GLchar* varyings[3] = {
            "info",
            "vel",
            "pos"
        };

        GLint vs = glCreateShader( GL_VERTEX_SHADER );
        glShaderSource( vs,1, vs_src, NULL );
        compileShader( vs, "particle animation vertex shader" );

        GLint gs = glCreateShader( GL_GEOMETRY_SHADER_EXT );
        glShaderSource( gs, 2, gs_src, NULL );
        compileShader( gs, "particle animation geometry shader" );

        anim_p = glCreateProgram();
        glAttachShader( anim_p, vs );
        glAttachShader( anim_p, gs );
        glTransformFeedbackVaryings( anim_p, 3, varyings, GL_INTERLEAVED_ATTRIBS );
        glBindAttribLocation( anim_p, 0, "vbo_texcoord" );
        glBindAttribLocation( anim_p, 1, "vbo_normal" );
        glBindAttribLocation( anim_p, 2, "vbo_position" );
        linkProgram( anim_p, "particle animation program" );

        anim_loc_dt = glGetUniformLocation( anim_p, "dt" );
        anim_loc_iso = glGetUniformLocation( anim_p, "iso" );
        anim_loc_P = glGetUniformLocation( anim_p, "P" );
        anim_loc_MV = glGetUniformLocation( anim_p, "MV" );
        anim_loc_MV_inv = glGetUniformLocation( anim_p, "MV_inv" );
        anim_loc_NM = glGetUniformLocation( anim_p, "NM" );
        anim_loc_shape = glGetUniformLocation( anim_p, "shape" );

        glDeleteShader( vs );
        glDeleteShader( gs );

    }
    // --- set up particle billboard render program ----------------------------
    {
        const GLchar* vs_src[1] = {
            resources::particles_billboard_vs_150.c_str()
        };
        const GLchar* gs_src[1] = {
            resources::particles_billboard_gs_150.c_str(),
        };
        const GLchar* fs_src[1] = {
            resources::particles_billboard_fs_150.c_str()
        };

        GLint vs = glCreateShader( GL_VERTEX_SHADER );
        glShaderSource( vs, 1, vs_src, NULL );
        compileShader( vs, "particle billboard render vertex shader" );

        GLint gs = glCreateShader( GL_GEOMETRY_SHADER_EXT );
        glShaderSource( gs, 1, gs_src, NULL );
        compileShader( gs, "particle billboard render geometry shader" );

        GLint fs = glCreateShader( GL_FRAGMENT_SHADER );
        glShaderSource( fs, 1, fs_src, NULL );
        compileShader( fs, "particle billboard render fragment shader" );

        billboard_p = glCreateProgram();
        glAttachShader( billboard_p, vs );
        glAttachShader( billboard_p, gs );
        glAttachShader( billboard_p, fs );
        glBindFragDataLocation( billboard_p, 0, "fragment" );
        linkProgram( billboard_p, "particle billboard render program" );
        billboard_loc_P = glGetUniformLocation( billboard_p, "P" );
        billboard_loc_color = glGetUniformLocation( billboard_p, "color" );
    }
    // --- set up buffer to feedback triangles ---------------------------------
    {
        mc_tri_vbo_N = 3*1000;
        glGenVertexArrays( 1, &mc_tri_vao );
        glGenBuffers( 1, &mc_tri_vbo );

        glBindVertexArray( mc_tri_vao );
        glBindBuffer( GL_ARRAY_BUFFER, mc_tri_vbo );
        glBufferData( GL_ARRAY_BUFFER,
                      (3+3)*mc_tri_vbo_N * sizeof(GLfloat),
                      NULL,
                      GL_DYNAMIC_COPY );
        glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*6, (const GLvoid*)(sizeof(GLfloat)*0) );
        glEnableVertexAttribArray( 0 );
        glVertexAttribPointer( 1, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*6, (const GLvoid*)(sizeof(GLfloat)*3) );
        glEnableVertexAttribArray( 1 );

        glBindBuffer( GL_ARRAY_BUFFER, 0 );
        glBindVertexArray( 0 );
    }
    // --- set up two buffers to feedback particles ----------------------------
    {
        particles_vbo_p = 0;
        particles_vbo_n = 0;
        particles_vbo_N = 20000;
        glGenVertexArrays( 2, particles_vao );
        glGenBuffers( 2, &particles_vbo[0] );
        for(int i=0; i<2; i++) {
            glBindVertexArray( particles_vao[i] );

            glBindBuffer( GL_ARRAY_BUFFER, particles_vbo[i] );
            glBufferData( GL_ARRAY_BUFFER,
                          (2+3+3)*particles_vbo_N * sizeof(GLfloat),
                          NULL,
                          GL_DYNAMIC_COPY );
            glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*8, (const GLvoid*)(sizeof(GLfloat)*0) );
            glEnableVertexAttribArray( 0 );
            glVertexAttribPointer( 1, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*8, (const GLvoid*)(sizeof(GLfloat)*2) );
            glEnableVertexAttribArray( 1 );
            glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*8, (const GLvoid*)(sizeof(GLfloat)*5) );
            glEnableVertexAttribArray( 2 );
        }
        glBindBuffer( GL_ARRAY_BUFFER, 0 );
        glBindVertexArray( 0 );
    }

    // --- set up queries to track number of primitives produced ----------------

    // primitives produced by emitter
    glGenQueries( 1, &emitter_query );

    // primitives survived animation shader
    glGenQueries( 1, &anim_query );
}

// -----------------------------------------------------------------------------
void
render( float t,
        float dt,
        float fps,
        const GLfloat* P,
        const GLfloat* MV,
        const GLfloat* PM,
        const GLfloat *NM,
        const GLfloat* MV_inv )
{
    static int threshold = 500;
    if( t < 1e-6 ) {
        particles_vbo_n = 0.0f;
        threshold = 500;
        particles_vbo_p = 0;
        srand(42);
        std::cerr << "reset\n";
    }

    // --- clear screen and set up view ----------------------------------------
    glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

    // ---- calc coefficients of shape -----------------------------------------

    // the algebraic shapes we morph between
    static GLfloat C[7][12] = {
        // x^5,	x^4,	y^4,	z^4,	x^2y^2,	x^2z^2,	y^2z^2,	xyz,	x^2,	y^2,	z^2,	1
        // helix
        { 0.0,	-2.0,	0.0,	0.0,	0.0,	0.0,	-1.0,	0.0,	6.0,	0.0,	0.0,	0.0 },
        // some in-between shapes
        { 0.0,	8.0,	0.5,	0.5,	4.0,	4.0,	-1.4,	0.0,	0.0,	0.0,	0.0,	0.0 },
        { 0.0,  16.0,	1.0,	1.0,	8.0,	8.0,	-2.0,	0.0,	-6.0,	0.0,	0.0,	0.0 },
        // daddel
        { 0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	1.0,	1.0,	0.3,	-0.95 },
        // torus
        { 0.0,	1.0,	1.0,	1.0,	2.0,	2.0,	2.0,	0.0,	-1.01125, -1.01125, 0.94875, 0.225032 },
        // kiss
        { -0.5,	-0.5,	0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	1.0,	1.0,	0.0 },
        // cayley
        { 0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	0.0,	16.0,	4.0,	4.0,	4.0,	-1.0 },
    };

    GLfloat CC[12];
    int shape1 = static_cast<int>(t/13.0f) % 7;
    int shape2 = static_cast<int>((t+1.0)/13.0f) % 7;
    float u = fmodf( (t+1.0f), 13.0f );
    for(int i=0; i<12; i++) {
        CC[i] = (1.0f-u)*C[shape1][i] + u*C[shape2][i];
    }


    // --- build HistoPyramid --------------------------------------------------
    double iso = 0.001f;//sin(0.1*t);//4.0*fabs(sin(0.4*t));
    GLuint builder = HPMCgetBuilderProgram( hpmc_h );
    glUseProgram( builder );
    glUniform1fv( glGetUniformLocation( builder, "shape" ), 12, &CC[0] );
    HPMCbuildIsoSurface( hpmc_h, iso );

    // Get number of vertices in MC triangulation, forces CPU-GPU sync.
    GLsizei N = HPMCacquireNumberOfVertices( hpmc_h );

    // Resize triangulatin VBO to be large enough to hold the triangulation.
    if( mc_tri_vbo_N < N ) {
        mc_tri_vbo_N = static_cast<GLsizei>( 1.1f*static_cast<float>(N) );
        cerr << "resizing mc_tri_vbo to hold "
             << mc_tri_vbo_N << " vertices." << endl;

        glBindBuffer( GL_ARRAY_BUFFER, mc_tri_vbo );
        glBufferData( GL_ARRAY_BUFFER,
                      (3+3) * mc_tri_vbo_N * sizeof(GLfloat),
                      NULL,
                      GL_DYNAMIC_COPY );
        glBindBuffer( GL_ARRAY_BUFFER, 0 );
    }

    glEnable( GL_DEPTH_TEST );

    // --- render solid surface ------------------------------------------------
    // render to screen and store triangles into mc_tri_vbo buffer. Since we
    // know the number of triangles that we are going to render, we don't have
    // to do a query on the result.
    glUseProgram( onscreen_p );

    glUniformMatrix4fv( onscreen_loc_P, 1, GL_FALSE, P );
    glUniformMatrix4fv( onscreen_loc_M, 1, GL_FALSE, MV );
    glUniformMatrix3fv( onscreen_loc_NM, 1, GL_FALSE, NM );
    glUniform1fv( onscreen_loc_shape, 12, CC );
    glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, mc_tri_vbo );
    HPMCextractVerticesTransformFeedback( hpmc_th, GL_FALSE );


    // --- emit particles ------------------------------------------------------
    // Threshold such that only every n'th triangle produce a particle. This
    // threshold is adjusted according to the number of points actually
    // produced.

    int off = (int)(threshold*(rand()/(RAND_MAX+1.0f) ) );
    glUseProgram( emitter_p );
    glUniformMatrix4fv( emitter_loc_P, 1, GL_FALSE, P );
    glUniform1i( emitter_loc_offset, off );
    glUniform1i( emitter_loc_threshold, threshold );

    // Store emitted particles in the beginning of next frame's particle
    // buffer.
    glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, particles_vbo[ (particles_vbo_p+1)%2 ] );

    // Set up rendering of the triangles stored in the previous transform
    // feedback step.

    glBindVertexArray( mc_tri_vao );
    glEnable( GL_RASTERIZER_DISCARD );
    glBeginQuery( GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, emitter_query );
    glBeginTransformFeedback( GL_POINTS );
    glDrawArrays( GL_TRIANGLES, 0, N );
    glEndTransformFeedback();
    glEndQuery( GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN );
    glDisable( GL_RASTERIZER_DISCARD );


    // Get hold of number of primitives produced (number of new particles).
    GLuint emitter_result;
    glGetQueryObjectuiv( emitter_query, GL_QUERY_RESULT, &emitter_result );


    // Adjust threshold, try to keep a steady flow of newly generated particles
    float particles_per_sec = emitter_result/max(1e-5f,dt);

    if( particles_per_sec < particle_flow-100 ) {
        threshold = max(1, static_cast<int>(0.5*threshold) );
    }
    else if( particles_per_sec > particle_flow+100 ) {
        threshold = min( 100000, static_cast<int>(10.1*threshold) );
    }

    // --- animate and render particles ----------------------------------------
    // We animate the particles from the previous frame, deleting the ones that
    // is too old, and concatenate the result behind the newly created
    // particles. We also pass the particles on to the fragment shader for
    // rendering. To get more fancy effects, one would add a dedicated rendering
    // pass that expanded every particle into a billboard and do some blending.
    glUseProgram( anim_p );
    glUniform1fv( anim_loc_shape, 12, CC );
    glUniform1f( anim_loc_dt, dt );
    glUniform1f( anim_loc_iso, iso );
    glUniformMatrix4fv( anim_loc_P, 1, GL_FALSE, P );
    glUniformMatrix4fv( anim_loc_MV, 1, GL_FALSE, MV );
    glUniformMatrix4fv( anim_loc_MV_inv, 1, GL_FALSE, MV_inv );
    glUniformMatrix3fv( anim_loc_NM, 1, GL_FALSE, NM );

    // Output after the results of the emitter.
    glBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                       particles_vbo[ (particles_vbo_p+1)%2 ],
                       8*sizeof(GLfloat)*emitter_result,
                       8*sizeof(GLfloat)*(particles_vbo_N-emitter_result) );

    // Render previous frame's particles
    glBindVertexArray( particles_vao[ particles_vbo_p] );
    glEnable( GL_RASTERIZER_DISCARD );
    glBeginQuery( GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, anim_query );
    glBeginTransformFeedback( GL_POINTS );
    glDrawArrays( GL_POINTS, 0, particles_vbo_n );
    glEndTransformFeedback();
    glEndQuery( GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN );
    glDisable( GL_RASTERIZER_DISCARD );

    // Get hold of the number of particles that didn't die of old age.
    glGetQueryObjectuiv( anim_query, GL_QUERY_RESULT, &anim_result );

    // Update buffer pointer and number of particles in this frame's buffer.
    particles_vbo_p = (particles_vbo_p+1)%2;
    particles_vbo_n = anim_result + emitter_result;

    // --- render all particles as billboards ----------------------------------
    glUseProgram( billboard_p );
    glUniformMatrix4fv( billboard_loc_P, 1, GL_FALSE, P );
    glUniform3f( billboard_loc_color, 1.f, 1.f, 1.f );

    glDepthMask( GL_FALSE );
    glEnable( GL_BLEND );
    glBlendFunc( GL_ONE, GL_ONE );
    glBindVertexArray( particles_vao[ particles_vbo_p ] );
    glDrawArrays( GL_POINTS, 0, particles_vbo_n );
    glDisable( GL_BLEND );
    glDepthMask( GL_TRUE );

    glBindVertexArray( 0 );

}

const std::string
infoString( float fps )
{
    std::stringstream o;
    o << std::setprecision(5) << fps << " fps, "
      << volume_size_x << 'x'
      << volume_size_y << 'x'
      << volume_size_z << " samples, "
      << (int)( ((volume_size_x-1)*(volume_size_y-1)*(volume_size_z-1)*fps)/1e6 )
      << " MVPS, "
      << (HPMCacquireNumberOfVertices( hpmc_h )/3)
      << " triangles, "
      << anim_result << " particles";
    return o.str();
}
